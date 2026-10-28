#include "ai/application/verification/ManifestVerificationAttributionService.hpp"

#include "ai/application/workspace/FilesystemPathUtils.hpp"
#include "ai/application/verification/ManifestVerificationRepairability.hpp"

#include <algorithm>
#include <cctype>
#include <map>
#include <set>
#include <tuple>
#include <unordered_map>
#include <unordered_set>

namespace ai::application {
namespace {

struct Blocked {
    std::string code;
    std::string message;
};

constexpr std::string_view kOwnershipMismatchCode =
    "verification_continuation.attribution_source_changed";
constexpr std::string_view kOwnershipMismatchMessage =
    "The source ledger task ownership does not match the exact verified proposal set";

bool isLegacyOwnershipMismatch(const ManifestVerificationAttributionCheckpoint& checkpoint)
{
    return checkpoint.status == ManifestVerificationAttributionStatus::Blocked
        && checkpoint.blockCode == kOwnershipMismatchCode
        && checkpoint.blockMessage == kOwnershipMismatchMessage;
}

std::optional<std::string> normalizedFindingPath(const std::string& fileKey)
{
    std::string relative = fileKey;
    bool candidateRelative = false;
    for (const std::string_view prefix : {std::string_view("<candidate>/"),
             std::string_view("<candidate>\\")}) {
        if (relative.starts_with(prefix)) {
            relative = relative.substr(prefix.size());
            candidateRelative = true;
            break;
        }
    }
    if (!candidateRelative || relative.empty()) return std::nullopt;
    const auto path = detail::pathFromUtf8(relative);
    const auto key = detail::normalizedPathKey(path);
    if (key.empty() || key == "." || key.starts_with("../")
        || key.find("/../") != std::string::npos || key.find(':') != std::string::npos) {
        return std::nullopt;
    }
    return key;
}

bool sameCriterionShape(const ManifestProposalSetVerification& left,
    const ManifestProposalSetVerification& right)
{
    if (left.criteria.size() != right.criteria.size()
        || left.checks.size() != right.checks.size()) return false;
    for (std::size_t i = 0; i < left.criteria.size(); ++i) {
        if (left.criteria[i].requirementId != right.criteria[i].requirementId
            || left.criteria[i].required != right.criteria[i].required) return false;
    }
    for (std::size_t i = 0; i < left.checks.size(); ++i) {
        if (left.checks[i].profileId != right.checks[i].profileId
            || left.checks[i].kind != right.checks[i].kind
            || left.checks[i].commandIdentity != right.checks[i].commandIdentity) return false;
    }
    return true;
}

// MW-P5.6D: a fingerprint pair is accepted either because it is literally
// identical (the ordinary case, unaffected by this package) or because an
// immutable normalization proof for this exact source Run names exactly
// this durable-fingerprint-to-observed-fingerprint transition. Never a
// blanket allowance - a proof for a DIFFERENT durable/normalized pair (e.g.
// stale from a since-superseded workspace state) does not match here.
bool normalizedFingerprintMatches(const std::string& durableFingerprint,
    const std::string& observedFingerprint,
    const std::optional<ManifestVerificationBaselineNormalization>& normalization)
{
    if (durableFingerprint == observedFingerprint) return true;
    return normalization.has_value()
        && normalization->durableBaselineFingerprint == durableFingerprint
        && normalization->normalizedBaselineFingerprint == observedFingerprint;
}

std::optional<std::string> sourceIdentityMismatch(const ManifestProposalSetVerification& durable,
    const ManifestProposalSetVerification& candidate,
    const std::optional<ManifestVerificationBaselineNormalization>& normalization)
{
    const bool baselineFingerprintOk = normalizedFingerprintMatches(
        durable.baselineFingerprint, candidate.baselineFingerprint, normalization);
    // overlayFingerprint mixes baseline file content into its hash, so it
    // unavoidably changes as a direct CONSEQUENCE of a proven baseline
    // normalization - it is only compared strictly when the baseline
    // fingerprint itself matched WITHOUT relying on that proof (i.e. no
    // normalization was actually exercised for this pair).
    const bool baselineWasNormalized =
        baselineFingerprintOk && durable.baselineFingerprint != candidate.baselineFingerprint;
    const bool overlayFingerprintOk =
        baselineWasNormalized || durable.overlayFingerprint == candidate.overlayFingerprint;
    if (durable.workspaceIdentity != candidate.workspaceIdentity) return "workspace_identity";
    if (!baselineFingerprintOk) return "baseline_fingerprint";
    if (durable.orderedProposalIds != candidate.orderedProposalIds) return "proposal_ids";
    if (durable.orderedProposalContentHashes != candidate.orderedProposalContentHashes)
        return "proposal_content_hashes";
    if (!overlayFingerprintOk) return "overlay_fingerprint";
    if (durable.proposalSafetyStatus != candidate.proposalSafetyStatus)
        return "proposal_safety_status";
    if (durable.buildLintTestStatus != candidate.buildLintTestStatus)
        return "build_lint_test_status";
    if (durable.acceptanceStatus != candidate.acceptanceStatus) return "acceptance_status";
    if (durable.criteria.size() != candidate.criteria.size()) return "criteria_count";
    for (std::size_t i = 0; i < durable.criteria.size(); ++i) {
        const auto& a = durable.criteria[i];
        const auto& b = candidate.criteria[i];
        if (a.requirementId != b.requirementId || a.required != b.required) {
            return "criterion_identity:" + a.requirementId;
        }
        if (a.status != b.status) {
            return "criterion_status:" + a.requirementId + ":"
                + toString(a.status) + "->" + toString(b.status);
        }
    }
    return std::nullopt;
}

struct TaskProposalOwnership {
    std::string taskId;
    ChangeSetId dispositionProposalId;
    const ManifestWorkLedgerEntry* sourceLedger {nullptr};
};

struct VerifiedOwnershipResolution {
    std::unordered_map<std::string, ChangeSetId> proposalByTask;
    Blocked blocked;
};

struct VerifiedAncestorResolution {
    std::optional<ChangeSetId> proposalId;
    bool corrupt {false};
};

// A bounded verification-repair wave may persist completed successor task
// dispositions and then terminate before it can run a new final combined
// verification (for example, when the Run-wide turn ceiling is reached).
// In that honest state, Work Ledger verification still names the primary
// proposal while the task disposition names its later successor. The
// immutable proposalLineage snapshot is the only authority that may join
// those generations. Walk only from the disposition backwards through
// supersedesChangeSetId; never accept a sibling, arbitrary proposalIds member,
// or a descendant that the verified identity did not itself name.
VerifiedAncestorResolution resolveVerifiedAncestor(
    const ManifestWorkLedgerEntry& source,
    const ChangeSetId& dispositionProposalId,
    const std::unordered_set<ChangeSetId>& verifiedProposalIds)
{
    if (verifiedProposalIds.contains(dispositionProposalId)) {
        return {dispositionProposalId, false};
    }

    std::unordered_set<ChangeSetId> durableProposalIds;
    durableProposalIds.reserve(source.proposalIds.size());
    for (const auto& proposalId : source.proposalIds) {
        if (!durableProposalIds.insert(proposalId).second) return {{}, true};
    }

    std::unordered_map<ChangeSetId, const ManifestWorkLedgerProposalLineage*> lineageById;
    lineageById.reserve(source.proposalLineage.size());
    for (const auto& item : source.proposalLineage) {
        if (item.changeSetId.empty()
            || (!durableProposalIds.empty()
                && !durableProposalIds.contains(item.changeSetId))
            || !lineageById.emplace(item.changeSetId, &item).second) {
            return {{}, true};
        }
    }

    if (!durableProposalIds.empty()
        && !durableProposalIds.contains(dispositionProposalId)) {
        return {{}, true};
    }

    std::unordered_set<ChangeSetId> visited;
    ChangeSetId current = dispositionProposalId;
    for (std::size_t depth = 0; depth <= source.proposalLineage.size(); ++depth) {
        if (!visited.insert(current).second) return {{}, true};
        const auto currentLineage = lineageById.find(current);
        if (currentLineage == lineageById.end()
            || !currentLineage->second->supersedesChangeSetId.has_value()) {
            return {{}, false};
        }
        current = *currentLineage->second->supersedesChangeSetId;
        // The verified ancestor itself must also be a real member of this
        // ledger's immutable lineage snapshot. A dangling predecessor id is
        // not sufficient evidence even if it happens to equal a verified id.
        if (!lineageById.contains(current)
            || (!durableProposalIds.empty() && !durableProposalIds.contains(current))) {
            return {{}, true};
        }
        if (verifiedProposalIds.contains(current)) return {current, false};
    }
    return {{}, true};
}

VerifiedOwnershipResolution resolveVerifiedProposalOwnership(
    const ManifestWorkLedgerEntry& ledger,
    const std::optional<ManifestWorkLedgerEntry>& originLedgerForMixedAnchor,
    const std::vector<ChangeSetId>& orderedVerifiedProposalIds)
{
    VerifiedOwnershipResolution result;
    std::unordered_set<ChangeSetId> verifiedProposalIds;
    verifiedProposalIds.reserve(orderedVerifiedProposalIds.size());
    for (const auto& proposalId : orderedVerifiedProposalIds) {
        if (!verifiedProposalIds.insert(proposalId).second) {
            result.blocked = {std::string(kOwnershipMismatchCode),
                std::string(kOwnershipMismatchMessage)};
            return result;
        }
    }

    std::vector<TaskProposalOwnership> ownership;
    std::unordered_set<std::string> seenTaskIds;
    std::unordered_set<ChangeSetId> seenDispositionProposalIds;
    const auto append = [&](const ManifestWorkLedgerEntry& source,
                            bool onlyMissingTasks) -> bool {
        for (const auto& disposition : source.taskDispositions) {
            if (!disposition.changeSetId.has_value()) continue;
            if (onlyMissingTasks && seenTaskIds.contains(disposition.taskId)) continue;
            if (!seenTaskIds.insert(disposition.taskId).second
                || !seenDispositionProposalIds.insert(*disposition.changeSetId).second) {
                return false;
            }
            ownership.push_back(
                {disposition.taskId, *disposition.changeSetId, &source});
        }
        return true;
    };

    if (!append(ledger, false)
        || (originLedgerForMixedAnchor.has_value()
            && !append(*originLedgerForMixedAnchor, true))) {
        result.blocked = {"verification_continuation.attribution_manifest_ambiguous",
            "The source ledger contains ambiguous task-to-proposal ownership"};
        return result;
    }

    std::unordered_set<ChangeSetId> assignedVerifiedProposalIds;
    for (const auto& item : ownership) {
        const auto resolved = resolveVerifiedAncestor(
            *item.sourceLedger, item.dispositionProposalId, verifiedProposalIds);
        if (resolved.corrupt) {
            result.blocked = {"verification_continuation.attribution_manifest_ambiguous",
                "The source ledger proposal lineage cannot prove unique task ownership"};
            return result;
        }
        if (!resolved.proposalId.has_value()) {
            result.blocked = {std::string(kOwnershipMismatchCode),
                std::string(kOwnershipMismatchMessage)};
            return result;
        }
        if (!assignedVerifiedProposalIds.insert(*resolved.proposalId).second
            || !result.proposalByTask.emplace(item.taskId, *resolved.proposalId).second) {
            result.blocked = {"verification_continuation.attribution_manifest_ambiguous",
                "The source ledger proposal lineage maps multiple tasks to one verified member"};
            return result;
        }
    }

    if (assignedVerifiedProposalIds != verifiedProposalIds) {
        result.proposalByTask.clear();
        result.blocked = {std::string(kOwnershipMismatchCode),
            std::string(kOwnershipMismatchMessage)};
    }
    return result;
}

using FindingKey = std::tuple<std::string, std::string, std::string, int, int,
    std::string, std::string>;

std::string canonicalFindingDiagnosticCode(const std::string& code)
{
    // The same qmllint finding is informational when a baseline contains
    // only out-of-scope warnings, but belongs to the ordinary blocking
    // diagnostic when a candidate also introduces any in-scope warning.
    // Diagnostic severity/scope packaging must not change finding identity.
    if (code == "verification.qml_warning_outside_task_scope") {
        return "verification.qml_warning";
    }
    return code;
}

std::optional<FindingKey> findingKey(const ManifestVerificationDiagnostic& diagnostic,
    const ManifestVerificationDiagnosticFinding& finding)
{
    if (!diagnostic.requirementId.has_value()) return std::nullopt;
    const auto path = normalizedFindingPath(finding.fileKey);
    if (!path.has_value()) return std::nullopt;
    return FindingKey {canonicalFindingDiagnosticCode(diagnostic.code),
        *diagnostic.requirementId, *path,
        finding.line, finding.column, finding.tag, finding.text};
}

// MW-P5.6C: ownerForPath's decision, distinguished so the caller can pick a
// distinct, stable error code instead of collapsing every failure mode into
// "no owner" (see ManifestVerificationAttributionService.hpp's class
// comment for why the workspace-scope cross-check exists at all).
enum class OwnerResolutionOutcome {
    Found,
    NotFound,
    // The frozen Manifest itself names more than one task as owner of the
    // same path. ExecutionManifestValidator's ownership_overlap check should
    // already prevent this at freeze time - kept as defense in depth, never
    // silently picking one owner over another.
    ManifestAmbiguous,
    // A frozen RunWorkspaceScope row exists for this Run but fails its own
    // identity/structural checks (wrong runId, schema version, or manifest
    // schema version; an entry names a taskId absent from the Manifest; or
    // the scope itself names more than one owner for the path). Never
    // partially trusted once found invalid - fail closed for every path
    // lookup against it, not just the malformed entry.
    ScopeInvalid,
    // The Manifest and a structurally valid frozen scope both name an
    // owner for the path, and they disagree (including the case where the
    // Manifest names an owner the scope's owned_write/planned_create
    // entries do not corroborate at all). Since the scope is derived
    // directly from the in-memory Manifest at freeze time
    // (RunWorkspaceScopeService::freezeForManifest), any such disagreement
    // means one of the two was corrupted after freeze - never silently
    // prefer either side.
    Conflict,
};

struct OwnerResolution {
    OwnerResolutionOutcome outcome {OwnerResolutionOutcome::NotFound};
    std::optional<std::string> ownerTaskId;
};

std::optional<std::string> manifestOwnerForPath(const std::string& path,
    const ExecutionManifest& manifest, bool& ambiguous)
{
    ambiguous = false;
    std::optional<std::string> owner;
    for (const auto& task : manifest.tasks) {
        const auto owns = [&](const auto& paths) {
            return std::any_of(paths.begin(), paths.end(), [&](const auto& candidate) {
                return detail::normalizedPathKey(candidate) == path;
            });
        };
        if (!owns(task.ownedPaths) && !owns(task.plannedCreatePaths)) continue;
        if (owner.has_value() && *owner != task.id) {
            ambiguous = true;
            return std::nullopt;
        }
        owner = task.id;
    }
    return owner;
}

struct ScopeOwnershipIndex {
    bool present {false};
    bool valid {true};
    std::map<std::string, std::string> ownerByPath;
};

bool isSha256(const std::string& value)
{
    return value.size() == 64
        && std::all_of(value.begin(), value.end(), [](const unsigned char ch) {
               return std::isxdigit(ch) != 0;
           });
}

std::optional<std::string> canonicalScopePath(const std::string& raw)
{
    if (raw.empty()) return std::nullopt;
    const auto key = detail::normalizedPathKey(detail::pathFromUtf8(raw));
    if (key.empty() || key == "." || key.starts_with("../")
        || key.find("/../") != std::string::npos || key.find(':') != std::string::npos
        || raw != key) {
        return std::nullopt;
    }
    return key;
}

ScopeOwnershipIndex validateScopeOwnership(const std::optional<RunWorkspaceScope>& optionalScope,
    const ExecutionManifest& manifest, const RunId& sourceRunId)
{
    ScopeOwnershipIndex result;
    if (!optionalScope.has_value()) return result;
    result.present = true;
    const auto& scope = *optionalScope;
    if (scope.runId != sourceRunId
        || scope.schemaVersion != std::string(kRunWorkspaceScopeSchemaVersion)
        || scope.manifestSchemaVersion != manifest.schemaVersion
        || scope.entries.size() > kRunWorkspaceScopeMaxEntries
        || scope.dependencyEdges.size() > kRunWorkspaceScopeMaxDependencyEdges) {
        result.valid = false;
        return result;
    }

    std::set<std::string> manifestTaskIds;
    for (const auto& task : manifest.tasks) {
        if (task.id.empty() || !manifestTaskIds.insert(task.id).second) {
            result.valid = false;
            return result;
        }
    }
    std::set<std::pair<std::string, std::string>> requiredReads;
    for (const auto& entry : scope.entries) {
        const auto path = canonicalScopePath(entry.relativePath);
        if (entry.origin != WorkspaceScopeOrigin::FrozenInitial || !path.has_value()
            || !manifestTaskIds.contains(entry.taskId) || entry.reason.empty()
            || entry.reason.size() > kRunWorkspaceScopeMaxReasonBytes
            || (entry.baselineHash.has_value() && !isSha256(*entry.baselineHash))) {
            result.valid = false;
            return result;
        }
        if (entry.role == WorkspaceScopeRole::OwnedWrite) {
            if (!entry.baselineHash.has_value()
                || !result.ownerByPath.emplace(*path, entry.taskId).second) {
                result.valid = false;
                return result;
            }
        } else if (entry.role == WorkspaceScopeRole::PlannedCreate) {
            if (entry.baselineHash.has_value()
                || !result.ownerByPath.emplace(*path, entry.taskId).second) {
                result.valid = false;
                return result;
            }
        } else if (entry.role == WorkspaceScopeRole::RequiredRead) {
            requiredReads.emplace(entry.taskId, *path);
        }
    }

    std::set<std::tuple<std::string, std::string, std::string>> edges;
    for (const auto& edge : scope.dependencyEdges) {
        const auto path = canonicalScopePath(edge.relativePath);
        if (!path.has_value() || edge.dependentTaskId == edge.providerTaskId
            || !manifestTaskIds.contains(edge.dependentTaskId)
            || !manifestTaskIds.contains(edge.providerTaskId)
            || !requiredReads.contains({edge.dependentTaskId, *path})) {
            result.valid = false;
            return result;
        }
        const auto owner = result.ownerByPath.find(*path);
        if (owner == result.ownerByPath.end() || owner->second != edge.providerTaskId
            || !edges.emplace(edge.dependentTaskId, edge.providerTaskId, *path).second) {
            result.valid = false;
            return result;
        }
    }
    return result;
}

// MW-P5.6C: the frozen Manifest stays the primary ownership source; the
// frozen AG-15 RunWorkspaceScope (when present) is a fail-closed cross-check
// and legacy fallback, never a silent override:
//   - no scope at all (a genuinely pre-AG-15 Run) -> Manifest alone decides,
//     exactly as before this package;
//   - scope present but structurally invalid -> ScopeInvalid, regardless of
//     what the Manifest says;
//   - Manifest names an owner and the valid scope corroborates the SAME
//     owner -> Found;
//   - Manifest names an owner but the valid scope names a different owner,
//     or no owner at all for this path -> Conflict (fail closed - never
//     trust the Manifest alone once a scope exists to check it against,
//     since the scope was derived from that same Manifest at freeze time);
//   - Manifest names no owner (e.g. a Run whose persisted Manifest lost its
//     AG-15 path-role fields to the persistence bug fixed in
//     dc0391a) but the valid scope names exactly one owner -> Found via
//     legacy fallback;
//   - neither side names an owner -> NotFound.
OwnerResolution resolveOwnerForPath(const std::string& path,
    const ExecutionManifest& manifest, const ScopeOwnershipIndex& scope)
{
    bool manifestAmbiguous = false;
    const auto manifestOwner = manifestOwnerForPath(path, manifest, manifestAmbiguous);
    if (manifestAmbiguous) return {OwnerResolutionOutcome::ManifestAmbiguous, std::nullopt};

    if (!scope.present) {
        if (!manifestOwner.has_value()) return {OwnerResolutionOutcome::NotFound, std::nullopt};
        return {OwnerResolutionOutcome::Found, manifestOwner};
    }
    if (!scope.valid) return {OwnerResolutionOutcome::ScopeInvalid, std::nullopt};
    const auto scopeOwnerIt = scope.ownerByPath.find(path);
    const std::optional<std::string> scopeOwner = scopeOwnerIt == scope.ownerByPath.end()
        ? std::nullopt : std::optional<std::string>(scopeOwnerIt->second);

    if (manifestOwner.has_value()) {
        if (!scopeOwner.has_value() || *scopeOwner != *manifestOwner)
            return {OwnerResolutionOutcome::Conflict, std::nullopt};
        return {OwnerResolutionOutcome::Found, manifestOwner};
    }
    if (scopeOwner.has_value()) return {OwnerResolutionOutcome::Found, scopeOwner};
    return {OwnerResolutionOutcome::NotFound, std::nullopt};
}

Blocked evaluate(const ManifestVerificationContinuationCheckpoint& admission,
    const ManifestWorkLedgerEntry& ledger,
    const ExecutionManifest& manifest,
    const std::optional<RunWorkspaceScope>& scope,
    const std::optional<ManifestVerificationBaselineNormalization>& normalization,
    const ManifestProposalSetVerification& baseline,
    const ManifestProposalSetVerification& candidate,
    // QML-P4R.2: the Work Ledger entry actually being attributed - the
    // origin Manifest Run for the ordinary path (equal to manifest.runId,
    // preserving the original identity check exactly), or a paid
    // continuation child's own anchor Run for attributeOrEvaluateForAnchor.
    // Never re-derived from manifest.runId internally, so the caller's own
    // choice of whose ledger to attribute is always explicit here.
    const RunId& expectedLedgerRunId,
    const std::optional<ManifestWorkLedgerEntry>& originLedgerForMixedAnchor,
    ManifestVerificationAttributionCheckpoint& result)
{
    if (admission.status != ManifestVerificationContinuationStatus::Admitted) {
        return {"verification_continuation.attribution_not_admitted",
            "Diagnostic attribution requires an admitted continuation checkpoint"};
    }
    if (admission.sourceProjectId != ledger.projectId || ledger.runId != expectedLedgerRunId
        || !ledger.verification.has_value()) {
        return {"verification_continuation.attribution_identity_mismatch",
            "Source Run, Manifest, ledger, and admission identities do not match"};
    }
    if (normalization.has_value()) {
        const auto validNormalization =
            validateManifestVerificationBaselineNormalization(*normalization);
        if (!validNormalization.success() || normalization->sourceRunId != manifest.runId) {
            return {"verification_continuation.attribution_normalization_corrupt",
                "The baseline-normalization proof does not match its source Run or policy"};
        }
    }
    const auto scopeOwnership = validateScopeOwnership(scope, manifest, manifest.runId);
    if (!scopeOwnership.valid) {
        return {"verification_continuation.attribution_scope_invalid",
            "The frozen workspace scope failed its immutable identity or structural contract"};
    }
    const auto validBaseline = validateManifestProposalSetVerification(baseline);
    const auto validCandidate = validateManifestProposalSetVerification(candidate);
    if (!validBaseline.success() || !validCandidate.success()) {
        return {"verification_continuation.attribution_verification_corrupt",
            "A Backend verification result is structurally invalid"};
    }
    const auto& durable = *ledger.verification;
    if (const auto mismatch = sourceIdentityMismatch(durable, candidate, normalization)) {
        return {"verification_continuation.attribution_source_changed",
            "The reconstructed candidate does not match the durable source verification identity "
            "(" + *mismatch + ")"};
    }
    if (baseline.workspaceIdentity != durable.workspaceIdentity
        || !normalizedFingerprintMatches(
            durable.baselineFingerprint, baseline.baselineFingerprint, normalization)
        || !baseline.orderedProposalIds.empty()
        || !baseline.orderedProposalContentHashes.empty()
        || !sameCriterionShape(baseline, candidate)) {
        return {"verification_continuation.attribution_baseline_mismatch",
            "The source-only verification did not use the same Backend-resolved contract"};
    }

    std::unordered_map<std::string, std::string> requirementOwner;
    for (const auto& task : manifest.tasks) {
        if (!task.acceptanceContract.has_value()) continue;
        for (const auto& requirement : task.acceptanceContract->requirements) {
            if (!requirementOwner.emplace(requirement.id, task.id).second) {
                return {"verification_continuation.attribution_manifest_ambiguous",
                    "The frozen Manifest contains a duplicate requirement identity"};
            }
        }
    }
    // QML-P4R.2: a mixed-run anchor's own ledger.taskDispositions covers
    // only the subset of tasks it itself repaired; origin dispositions fill
    // only missing tasks. A partially completed bounded repair wave can also
    // leave a task disposition at a genuine successor while the last
    // authoritative combined verification still names its predecessor.
    // Resolve both shapes through immutable lineage, one-to-one, and project
    // findings back onto the exact verified member - never the newer,
    // unverified successor.
    auto ownership = resolveVerifiedProposalOwnership(
        ledger, originLedgerForMixedAnchor, candidate.orderedProposalIds);
    if (!ownership.blocked.code.empty()) return ownership.blocked;
    const auto& proposalByTask = ownership.proposalByTask;

    std::set<FindingKey> baselineFindings;
    for (const auto& diagnostic : baseline.diagnostics) {
        for (const auto& finding : diagnostic.structuredFindings) {
            if (const auto key = findingKey(diagnostic, finding)) baselineFindings.insert(*key);
        }
    }

    constexpr std::size_t kMaximumAttributions = 256;
    std::set<FindingKey> attributedFindingKeys;
    std::set<std::pair<std::string, std::string>> cascades;
    for (const auto& diagnostic : candidate.diagnostics) {
        if (!diagnostic.requirementId.has_value()) {
            return {"verification_continuation.attribution_diagnostic_unmapped",
                "A verification diagnostic is not bound to a requirement"};
        }
        if (diagnostic.code == "verification.qml_warning_baseline_debt") {
            // This diagnostic is emitted only by the shared baseline-delta
            // resolver after a multiset match. Its code intentionally
            // differs from the baseline verifier's historical
            // qml_warning/outside_task_scope code, so the generic FindingKey
            // comparison below cannot re-prove it by diagnostic-code
            // equality. Preserve the resolver's already fail-closed proof,
            // while still validating every projected path here.
            if (diagnostic.structuredFindings.empty()) {
                return {"verification_continuation.attribution_diagnostic_unmapped",
                    "A proven baseline-debt diagnostic has no structured file provenance"};
            }
            for (const auto& finding : diagnostic.structuredFindings) {
                if (result.findings.size() >= kMaximumAttributions) {
                    return {"verification_continuation.attribution_limit_exceeded",
                        "The diagnostic attribution count exceeds the bounded limit"};
                }
                const auto key = findingKey(diagnostic, finding);
                if (!key.has_value()) {
                    return {"verification_continuation.attribution_path_invalid",
                        "A diagnostic does not contain a safe candidate-relative path"};
                }
                if (!attributedFindingKeys.insert(*key).second) continue;
                ManifestVerificationFindingAttribution item;
                item.diagnosticCode = diagnostic.code;
                item.requirementId = *diagnostic.requirementId;
                item.relativePath = std::get<2>(*key);
                item.line = finding.line;
                item.column = finding.column;
                item.tag = finding.tag;
                item.kind = ManifestVerificationFindingAttributionKind::
                    PreExistingVerificationDebt;
                result.findings.push_back(std::move(item));
            }
            continue;
        }
        if (isIgnorableNonRepairDiagnosticCode(diagnostic.code)) {
            if (!cascades.emplace(diagnostic.code, *diagnostic.requirementId).second) continue;
            ManifestVerificationFindingAttribution item;
            item.diagnosticCode = diagnostic.code;
            item.requirementId = *diagnostic.requirementId;
            item.kind = ManifestVerificationFindingAttributionKind::IgnorableCascade;
            result.findings.push_back(std::move(item));
            continue;
        }
        if (!isContentRepairableDiagnosticCode(diagnostic.code)
            || diagnostic.structuredFindings.empty()) {
            return {"verification_continuation.attribution_diagnostic_unmapped",
                "A content-repairable diagnostic has no structured file provenance"};
        }
        for (const auto& finding : diagnostic.structuredFindings) {
            if (result.findings.size() >= kMaximumAttributions) {
                return {"verification_continuation.attribution_limit_exceeded",
                    "The diagnostic attribution count exceeds the bounded limit"};
            }
            const auto key = findingKey(diagnostic, finding);
            if (!key.has_value()) {
                return {"verification_continuation.attribution_path_invalid",
                    "A diagnostic does not contain a safe candidate-relative path"};
            }
            if (!attributedFindingKeys.insert(*key).second) continue;
            ManifestVerificationFindingAttribution item;
            item.diagnosticCode = diagnostic.code;
            item.requirementId = *diagnostic.requirementId;
            item.relativePath = std::get<2>(*key);
            item.line = finding.line;
            item.column = finding.column;
            item.tag = finding.tag;
            if (baselineFindings.contains(*key)) {
                item.kind = ManifestVerificationFindingAttributionKind::PreExistingVerificationDebt;
            } else {
                const auto resolution = resolveOwnerForPath(
                    item.relativePath, manifest, scopeOwnership);
                switch (resolution.outcome) {
                case OwnerResolutionOutcome::NotFound:
                    return {"verification_continuation.attribution_outside_manifest",
                        "A candidate-introduced diagnostic is outside the frozen Manifest write "
                        "scope (" + item.relativePath + ")"};
                case OwnerResolutionOutcome::ManifestAmbiguous:
                    return {"verification_continuation.attribution_owner_ambiguous",
                        "The frozen Manifest declares more than one owner for the same write "
                        "path"};
                case OwnerResolutionOutcome::ScopeInvalid:
                    return {"verification_continuation.attribution_scope_invalid",
                        "The frozen workspace scope is missing required identity/structure, or "
                        "is internally ambiguous, for this path"};
                case OwnerResolutionOutcome::Conflict:
                    return {"verification_continuation.attribution_manifest_scope_conflict",
                        "The frozen Manifest and the frozen workspace scope disagree on which "
                        "task owns this path"};
                case OwnerResolutionOutcome::Found:
                    break;
                }
                const auto& owner = *resolution.ownerTaskId;
                const auto proposal = proposalByTask.find(owner);
                if (proposal == proposalByTask.end()) {
                    return {"verification_continuation.attribution_owner_missing_proposal",
                        "A diagnostic owner has no durable source proposal"};
                }
                item.ownerTaskId = owner;
                item.ownerProposalId = proposal->second;
                const auto requirement = requirementOwner.find(item.requirementId);
                item.kind = requirement != requirementOwner.end() && requirement->second == owner
                    ? ManifestVerificationFindingAttributionKind::IntegrationOwner
                    : ManifestVerificationFindingAttributionKind::SourceProposalOwner;
            }
            result.findings.push_back(std::move(item));
        }
    }
    if (result.findings.empty()) {
        return {"verification_continuation.attribution_empty",
            "The failed verification produced no attributable diagnostics"};
    }
    return {};
}

} // namespace

ManifestVerificationAttributionService::ManifestVerificationAttributionService(
    IExecutionManifestRepository& manifests,
    IManifestWorkLedgerRepository& workLedger,
    IManifestVerificationContinuationRepository& continuations,
    IRunWorkspaceScopeRepository& workspaceScopes,
    IManifestVerificationBaselineNormalizationRepository& baselineNormalizations)
    : manifests_(manifests), workLedger_(workLedger), continuations_(continuations)
    , workspaceScopes_(workspaceScopes), baselineNormalizations_(baselineNormalizations)
{
}

common::Result<ManifestVerificationAttributionCheckpoint>
ManifestVerificationAttributionService::attributeOrEvaluate(
    const RunId& sourceRunId,
    const ManifestProposalSetVerification& baselineVerification,
    const ManifestProposalSetVerification& candidateVerification)
{
    const auto existing = continuations_.findAttributionBySourceRun(sourceRunId);
    if (!existing.success())
        return common::Result<ManifestVerificationAttributionCheckpoint>::failure(*existing.error());
    const bool retryLegacyOwnershipMismatch = existing.value().has_value()
        && isLegacyOwnershipMismatch(*existing.value());
    if (existing.value().has_value() && !retryLegacyOwnershipMismatch)
        return common::Result<ManifestVerificationAttributionCheckpoint>::ok(*existing.value());

    const auto admission = continuations_.findBySourceRun(sourceRunId);
    const auto ledger = workLedger_.findEntry(sourceRunId);
    const auto manifest = manifests_.findForRun(sourceRunId);
    // MW-P5.6C: absence is not an error - a genuinely pre-AG-15 Run legally
    // has no frozen scope row, and ownership resolution falls back to the
    // Manifest alone exactly as before this package (see
    // resolveOwnerForPath's doc comment). Only a repository-level failure is
    // fatal here.
    const auto scope = workspaceScopes_.findForRun(sourceRunId);
    // MW-P5.6D: same "absence is not an error" reasoning as scope above - a
    // Run whose baseline never drifted (the overwhelming majority) legally
    // has no normalization proof.
    const auto normalization = baselineNormalizations_.findBySourceRun(sourceRunId);
    if (!admission.success())
        return common::Result<ManifestVerificationAttributionCheckpoint>::failure(*admission.error());
    if (!ledger.success())
        return common::Result<ManifestVerificationAttributionCheckpoint>::failure(*ledger.error());
    if (!manifest.success())
        return common::Result<ManifestVerificationAttributionCheckpoint>::failure(*manifest.error());
    if (!scope.success())
        return common::Result<ManifestVerificationAttributionCheckpoint>::failure(*scope.error());
    if (!normalization.success())
        return common::Result<ManifestVerificationAttributionCheckpoint>::failure(*normalization.error());
    if (!admission.value().has_value() || !ledger.value().has_value()
        || !manifest.value().has_value()) {
        return common::Result<ManifestVerificationAttributionCheckpoint>::failure({
            "verification_continuation.attribution_source_missing",
            "Admission, frozen Manifest, or Work Ledger source data is missing"});
    }

    ManifestVerificationAttributionCheckpoint checkpoint;
    checkpoint.id = "verification-attribution:" + sourceRunId;
    checkpoint.sourceRunId = sourceRunId;
    checkpoint.sourceProjectId = admission.value()->sourceProjectId;
    // Anchor the checkpoint to the already-durable source identity even when
    // the supplied reconstruction is corrupt/mismatched and the outcome is
    // Blocked. Never persist untrusted/mismatched identities as the anchor.
    checkpoint.baselineFingerprint = ledger.value()->verification.has_value()
        ? ledger.value()->verification->baselineFingerprint : "unavailable";
    checkpoint.sourceOverlayFingerprint = ledger.value()->verification.has_value()
        ? ledger.value()->verification->overlayFingerprint : "unavailable";
    checkpoint.createdAt = std::chrono::system_clock::now();
    const auto blocked = evaluate(*admission.value(), *ledger.value(), *manifest.value(),
        scope.value(), normalization.value(), baselineVerification, candidateVerification,
        manifest.value()->runId, std::nullopt, checkpoint);
    checkpoint.status = blocked.code.empty()
        ? ManifestVerificationAttributionStatus::Attributed
        : ManifestVerificationAttributionStatus::Blocked;
    checkpoint.blockCode = blocked.code;
    checkpoint.blockMessage = blocked.message;
    if (!blocked.code.empty()) checkpoint.findings.clear();

    // The old direct-id ownership comparison could durably cache a false
    // denial for a Run whose task disposition had advanced to a genuine
    // successor after the last completed combined verification. Preserve
    // that immutable row as audit history. Only this exact historical denial
    // is re-evaluated. Return the freshly computed in-memory result whether it
    // is Attributed or a different precise fail-closed denial: returning the
    // old row for every blocked outcome would mask the current blocker forever.
    // The legacy row is deliberately not rewritten; a crash before the caller
    // consumes this result simply re-evaluates the same immutable evidence.
    if (retryLegacyOwnershipMismatch) {
        return common::Result<ManifestVerificationAttributionCheckpoint>::ok(
            std::move(checkpoint));
    }

    const auto inserted = continuations_.insertAttribution(checkpoint);
    if (!inserted.success()) {
        if (inserted.error()->code == "manifest_verification_attribution.already_exists") {
            const auto raced = continuations_.findAttributionBySourceRun(sourceRunId);
            if (raced.success() && raced.value().has_value())
                return common::Result<ManifestVerificationAttributionCheckpoint>::ok(*raced.value());
        }
        return common::Result<ManifestVerificationAttributionCheckpoint>::failure(*inserted.error());
    }
    return common::Result<ManifestVerificationAttributionCheckpoint>::ok(std::move(checkpoint));
}

common::Result<ManifestVerificationAttributionCheckpoint>
ManifestVerificationAttributionService::attributeOrEvaluateForAnchor(
    const RunId& verificationAnchorRunId,
    const RunId& originManifestRunId,
    const ManifestProposalSetVerification& baselineVerification,
    const ManifestProposalSetVerification& candidateVerification)
{
    const auto existing = continuations_.findAttributionBySourceRun(verificationAnchorRunId);
    if (!existing.success())
        return common::Result<ManifestVerificationAttributionCheckpoint>::failure(*existing.error());
    const bool retryLegacyOwnershipMismatch = existing.value().has_value()
        && isLegacyOwnershipMismatch(*existing.value());
    if (existing.value().has_value() && !retryLegacyOwnershipMismatch)
        return common::Result<ManifestVerificationAttributionCheckpoint>::ok(*existing.value());

    // Admission, Manifest, frozen scope, and baseline normalization are all
    // properties of the ORIGIN Manifest Run itself - the anchor's own Work
    // Ledger entry is the actual durable evidence being attributed, and the
    // origin's OWN Work Ledger entry supplies task-ownership for whichever
    // tasks the anchor itself never repaired (see evaluate()'s own doc
    // comment on originLedgerForMixedAnchor).
    const auto admission = continuations_.findBySourceRun(originManifestRunId);
    const auto ledger = workLedger_.findEntry(verificationAnchorRunId);
    const auto originLedger = workLedger_.findEntry(originManifestRunId);
    const auto manifest = manifests_.findForRun(originManifestRunId);
    const auto scope = workspaceScopes_.findForRun(originManifestRunId);
    const auto normalization = baselineNormalizations_.findBySourceRun(originManifestRunId);
    if (!admission.success())
        return common::Result<ManifestVerificationAttributionCheckpoint>::failure(*admission.error());
    if (!ledger.success())
        return common::Result<ManifestVerificationAttributionCheckpoint>::failure(*ledger.error());
    if (!originLedger.success())
        return common::Result<ManifestVerificationAttributionCheckpoint>::failure(*originLedger.error());
    if (!manifest.success())
        return common::Result<ManifestVerificationAttributionCheckpoint>::failure(*manifest.error());
    if (!scope.success())
        return common::Result<ManifestVerificationAttributionCheckpoint>::failure(*scope.error());
    if (!normalization.success())
        return common::Result<ManifestVerificationAttributionCheckpoint>::failure(*normalization.error());
    if (!admission.value().has_value() || !ledger.value().has_value()
        || !originLedger.value().has_value() || !manifest.value().has_value()) {
        return common::Result<ManifestVerificationAttributionCheckpoint>::failure({
            "verification_continuation.attribution_source_missing",
            "Admission, frozen Manifest, or Work Ledger source data is missing"});
    }

    ManifestVerificationAttributionCheckpoint checkpoint;
    checkpoint.id = "verification-attribution:" + verificationAnchorRunId;
    checkpoint.sourceRunId = verificationAnchorRunId;
    checkpoint.sourceProjectId = admission.value()->sourceProjectId;
    checkpoint.baselineFingerprint = ledger.value()->verification.has_value()
        ? ledger.value()->verification->baselineFingerprint : "unavailable";
    checkpoint.sourceOverlayFingerprint = ledger.value()->verification.has_value()
        ? ledger.value()->verification->overlayFingerprint : "unavailable";
    checkpoint.createdAt = std::chrono::system_clock::now();
    const auto blocked = evaluate(*admission.value(), *ledger.value(), *manifest.value(),
        scope.value(), normalization.value(), baselineVerification, candidateVerification,
        verificationAnchorRunId, *originLedger.value(), checkpoint);
    checkpoint.status = blocked.code.empty()
        ? ManifestVerificationAttributionStatus::Attributed
        : ManifestVerificationAttributionStatus::Blocked;
    checkpoint.blockCode = blocked.code;
    checkpoint.blockMessage = blocked.message;
    if (!blocked.code.empty()) checkpoint.findings.clear();

    // Apply the same immutable-audit/current-result split to mixed anchors.
    // The anchor's historical row remains durable, while callers see the
    // result of evaluating the current origin/anchor evidence.
    if (retryLegacyOwnershipMismatch) {
        return common::Result<ManifestVerificationAttributionCheckpoint>::ok(
            std::move(checkpoint));
    }

    const auto inserted = continuations_.insertAttribution(checkpoint);
    if (!inserted.success()) {
        if (inserted.error()->code == "manifest_verification_attribution.already_exists") {
            const auto raced = continuations_.findAttributionBySourceRun(verificationAnchorRunId);
            if (raced.success() && raced.value().has_value())
                return common::Result<ManifestVerificationAttributionCheckpoint>::ok(*raced.value());
        }
        return common::Result<ManifestVerificationAttributionCheckpoint>::failure(*inserted.error());
    }
    return common::Result<ManifestVerificationAttributionCheckpoint>::ok(std::move(checkpoint));
}

} // namespace ai::application
