#include "ai/application/ManifestVerificationManualHandoffService.hpp"

#include "ai/application/planning/IOutcomeBriefTrigger.hpp"

#include "ai/application/ExecutionAcceptanceContract.hpp"
#include "ai/application/workspace/FilesystemPathUtils.hpp"
#include "ai/application/IWorkflowRunner.hpp"
#include "ai/application/IdGenerator.hpp"
#include "ai/application/verification/ManifestProposalOverlay.hpp"
#include "ai/application/verification/ManifestVerificationContinuationPreparationService.hpp"
#include "ai/application/verification/ManifestVerificationRepairability.hpp"
#include "ai/application/proposals/ProposedChangeSetParser.hpp"
#include "ai/application/verification/QmlLintDiagnosticPolicy.hpp"
#include "ai/application/Sha256.hpp"
#include "ai/application/workspace/WorkspaceBaselineProvenance.hpp"
#include "ai/application/workspace/WorkspaceReadPolicy.hpp"

#include <algorithm>
#include <fstream>
#include <functional>
#include <system_error>
#include <unordered_map>
#include <unordered_set>

#include <nlohmann/json.hpp>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#endif

namespace ai::application {
namespace {

template<class T>
common::Result<T> fail(std::string code, std::string message)
{
    return common::Result<T>::failure({std::move(code), std::move(message)});
}

class ScopeExit final {
public:
    explicit ScopeExit(std::function<void()> callback)
        : callback_(std::move(callback)) {}
    ~ScopeExit() { if (active_) callback_(); }
    void dismiss() noexcept { active_ = false; }
private:
    std::function<void()> callback_;
    bool active_ {true};
};

bool isOwnerFinding(const ManifestVerificationFindingAttribution& finding)
{
    return finding.kind == ManifestVerificationFindingAttributionKind::SourceProposalOwner
        || finding.kind == ManifestVerificationFindingAttributionKind::IntegrationOwner;
}

// QML-P4R.1 hardening: the ORIGINAL, never-superseded proposal a frozen
// member's own supersedesChangeSetId chain ultimately traces back to (itself,
// when it has never been amended by anything). Fails CLOSED rather than
// silently returning the last id reached: a missing/unreadable predecessor,
// a lineage that crosses project boundaries, a cycle, or a chain deeper than
// the bounded hop count are all distinct real corruption signals, never a
// legitimate "this is the root" answer - a genuine lineage in this codebase
// is never more than a handful of repair generations deep.
common::Result<ChangeSetId> resolveLineageRoot(
    ChangeProposalService& proposals, const ChangeSetId& proposalId)
{
    std::unordered_set<ChangeSetId> visited;
    std::optional<ProjectId> originProjectId;
    ChangeSetId current = proposalId;
    for (int hops = 0; hops < 32; ++hops) {
        if (!visited.insert(current).second) {
            return fail<ChangeSetId>("manual_handoff.lineage_cycle_detected",
                "A proposal's own supersedesChangeSetId lineage forms a cycle");
        }
        const auto proposal = proposals.findChangeSet(current);
        if (!proposal.success()) {
            return fail<ChangeSetId>("manual_handoff.lineage_predecessor_missing",
                "A proposal named by a supersedesChangeSetId lineage chain could not be read");
        }
        if (!originProjectId.has_value()) {
            originProjectId = proposal.value().projectId;
        } else if (proposal.value().projectId != *originProjectId) {
            return fail<ChangeSetId>("manual_handoff.lineage_project_mismatch",
                "A supersedesChangeSetId lineage chain crosses project boundaries");
        }
        if (!proposal.value().supersedesChangeSetId.has_value()) {
            return common::Result<ChangeSetId>::ok(current);
        }
        current = *proposal.value().supersedesChangeSetId;
    }
    return fail<ChangeSetId>("manual_handoff.lineage_too_deep",
        "A supersedesChangeSetId lineage chain exceeds the bounded hop limit");
}

struct CandidateOverlayCleanup {
    std::optional<ManifestProposalOverlayDescriptor> descriptor;
    bool keep {false};
    ~CandidateOverlayCleanup()
    {
        if (descriptor && !keep) static_cast<void>(ManifestProposalOverlay::cleanup(*descriptor));
    }
};

// Reads a candidate file's edited content, bounded to the same limit the
// provider-facing proposal-submission contract itself enforces - a manual
// edit is never trusted to be smaller just because a human made it.
common::Result<std::string> readEditedFile(const std::filesystem::path& path)
{
    std::error_code sizeError;
    const auto size = std::filesystem::file_size(path, sizeError);
    if (sizeError || size > kProposalSubmissionMaxContentBytes) {
        return fail<std::string>("manual_handoff.edited_file_too_large",
            "An edited candidate file could not be read or exceeds the bounded content limit");
    }
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        return fail<std::string>("manual_handoff.edited_file_unreadable",
            "An edited candidate file could not be opened");
    }
    std::string content(static_cast<std::size_t>(size), '\0');
    if (!content.empty()) stream.read(content.data(), static_cast<std::streamsize>(content.size()));
    if (!stream && !content.empty()) {
        return fail<std::string>("manual_handoff.edited_file_unreadable",
            "An edited candidate file could not be read completely");
    }
    return common::Result<std::string>::ok(std::move(content));
}

// Forward declaration: isReparsePoint() is defined further below in this
// same anonymous namespace, but resolveResetPath() needs it as its own
// belt-and-suspenders defense (see the comment inside).
bool isReparsePoint(const std::filesystem::path& path, std::error_code& error) noexcept;
bool isMissingFilesystemError(const std::error_code& error) noexcept;

common::Result<std::filesystem::path> resolveResetPath(
    const std::filesystem::path& candidateRoot, const std::string& relativePath)
{
    const auto relative = detail::pathFromUtf8(relativePath);
    if (relative.empty() || relative.is_absolute() || relative.has_root_name()
        || relative.has_root_directory()) {
        return fail<std::filesystem::path>("manual_handoff.reset_path_invalid",
            "A candidate reset path is not a safe relative path");
    }
    std::error_code error;
    const auto canonicalRoot = std::filesystem::canonical(candidateRoot, error);
    if (error) return fail<std::filesystem::path>("manual_handoff.reset_failed",
        "The candidate root could not be resolved for reset");

    // Belt-and-suspenders reparse-point check on the CALLER's own
    // (not-yet-canonicalized) path, mirroring WorkspacePathGuard's own
    // hasReparsePointBelowRoot(): canonical()/canonicalizeWithMissingSuffix()
    // below resolve and bound most symlinks/junctions via the containment
    // check, but some Windows STL implementations do not fully resolve a
    // relative symlink on a network share or a broken junction - inspecting
    // the already-resolved candidate would silently erase an in-workspace
    // reparse point from the path before it could be caught. Reset acts on a
    // candidate directory the user can freely edit outside the Backend, so
    // this check is not optional defense in depth here the way it might be
    // for purely Backend-authored paths.
    const auto rawCandidate = canonicalRoot / relative;
    std::size_t rootDepth = 0;
    for (auto it = canonicalRoot.begin(); it != canonicalRoot.end(); ++it) ++rootDepth;
    std::filesystem::path walked;
    std::size_t depth = 0;
    for (const auto& component : rawCandidate) {
        walked /= component;
        ++depth;
        if (depth <= rootDepth) continue;
        std::error_code reparseError;
        if (isReparsePoint(walked, reparseError)) {
            return fail<std::filesystem::path>("manual_handoff.reset_path_invalid",
                "A candidate reset path traverses a symbolic link or junction point");
        }
        if (reparseError && !isMissingFilesystemError(reparseError)) {
            return fail<std::filesystem::path>("manual_handoff.reset_failed",
                "A candidate reset path component could not be inspected safely");
        }
    }

    const auto resolved = detail::canonicalizeWithMissingSuffix(rawCandidate, error);
    if (error) return fail<std::filesystem::path>("manual_handoff.reset_failed",
        "A candidate path could not be resolved safely for reset");
    const auto within = resolved.lexically_relative(canonicalRoot);
    const auto withinKey = detail::normalizedPathKey(within);
    if (within.empty() || within.is_absolute() || withinKey == ".."
        || withinKey.starts_with("../")) {
        return fail<std::filesystem::path>("manual_handoff.reset_path_invalid",
            "A candidate reset path escapes the retained workspace");
    }
    return common::Result<std::filesystem::path>::ok(resolved);
}

common::Result<void> replaceCandidateFile(
    const std::filesystem::path& source, const std::filesystem::path& target)
{
    std::error_code error;
    if (!std::filesystem::is_regular_file(source, error) || error) {
        return fail<void>("manual_handoff.reset_source_mismatch",
            "A frozen candidate file is unavailable during reset");
    }
    std::filesystem::create_directories(target.parent_path(), error);
    if (error) return fail<void>("manual_handoff.reset_failed",
        "A candidate parent directory could not be restored");

    auto temporary = target;
    temporary += ".aio-reset-" + generateShortToken();
    struct TemporaryCleanup {
        std::filesystem::path path;
        ~TemporaryCleanup()
        {
            std::error_code ignored;
            static_cast<void>(std::filesystem::remove(path, ignored));
        }
    } cleanup {temporary};
    if (!std::filesystem::copy_file(
            source, temporary, std::filesystem::copy_options::overwrite_existing, error)
        || error) {
        return fail<void>("manual_handoff.reset_failed",
            "A frozen candidate file could not be staged for reset");
    }
#ifdef _WIN32
    if (!MoveFileExW(temporary.c_str(), target.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        return fail<void>("manual_handoff.reset_failed",
            "A staged candidate file could not replace the invalid edit");
    }
#else
    std::filesystem::rename(temporary, target, error);
    if (error) return fail<void>("manual_handoff.reset_failed",
        "A staged candidate file could not replace the invalid edit");
#endif
    cleanup.path.clear();
    return common::Result<void>::ok();
}

bool isActiveAttemptStatus(RunStatus status) noexcept
{
    return status == RunStatus::Queued
        || status == RunStatus::Running
        || status == RunStatus::Cancelling;
}

bool isMissingFilesystemError(const std::error_code& error) noexcept
{
    return error == std::errc::no_such_file_or_directory
        || error == std::errc::not_a_directory;
}

// std::filesystem::symlink_status() is sufficient for POSIX symlinks, but
// on Windows a directory junction is a reparse point that is not guaranteed
// to be reported as file_type::symlink by every STL implementation. Retention
// performs destructive cleanup, so consult the native attribute explicitly.
bool isReparsePoint(const std::filesystem::path& path, std::error_code& error) noexcept
{
    error.clear();
#ifdef _WIN32
    const auto attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        error = std::error_code(static_cast<int>(GetLastError()), std::system_category());
        return false;
    }
    return (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
#else
    const auto status = std::filesystem::symlink_status(path, error);
    return !error && std::filesystem::is_symlink(status);
#endif
}

// Preflight an orphan directory before remove_all(). directory_options::none
// never follows a directory symlink; encountering any symlink/reparse point
// leaves the entire orphan untouched. This also covers nested junctions, not
// just a reparse point used as the direct child selected by the sweep.
common::Result<bool> containsReparsePoint(const std::filesystem::path& root)
{
    std::error_code iteratorError;
    std::filesystem::recursive_directory_iterator iterator(
        root, std::filesystem::directory_options::none, iteratorError);
    const std::filesystem::recursive_directory_iterator end;
    if (iteratorError) {
        return fail<bool>("manual_handoff.retention_sweep_failed",
            "An orphan manual-handoff directory could not be inspected safely");
    }
    while (iterator != end) {
        std::error_code reparseError;
        if (isReparsePoint(iterator->path(), reparseError)) return common::Result<bool>::ok(true);
        if (reparseError) {
            return fail<bool>("manual_handoff.retention_sweep_failed",
                "An orphan manual-handoff directory could not be inspected safely");
        }
        iterator.increment(iteratorError);
        if (iteratorError) {
            return fail<bool>("manual_handoff.retention_sweep_failed",
                "An orphan manual-handoff directory could not be inspected safely");
        }
    }
    return common::Result<bool>::ok(false);
}

} // namespace

ManifestVerificationManualHandoffService::ManifestVerificationManualHandoffService(
    IRunRepository& runs, IProjectRepository& projects,
    IExecutionManifestRepository& manifests, IManifestWorkLedgerRepository& workLedger,
    IManifestVerificationManualHandoffRepository& manualHandoffs,
    IManifestVerificationContinuationRepository& continuations,
    ManifestVerificationContinuationService& admission,
    ManifestVerificationAttributionService& attribution,
    ChangeProposalService& proposals,
    IManifestVerificationCommandRunner& commandRunner,
    ManifestVerificationLegacyBaselineNormalizationService& baselineNormalization,
    std::filesystem::path manualHandoffRoot, std::filesystem::path ephemeralScratchRoot)
    : runs_(runs), projects_(projects), manifests_(manifests), workLedger_(workLedger),
      manualHandoffs_(manualHandoffs), continuations_(continuations), admission_(admission),
      attribution_(attribution), proposals_(proposals), commandRunner_(commandRunner),
      baselineNormalization_(baselineNormalization),
      // QML-P4R.3: no %TEMP% fallback - manualHandoffRoot is a required
      // constructor parameter (see the header's own doc comment on why).
      manualHandoffRoot_(std::move(manualHandoffRoot)),
      ephemeralScratchRoot_(ephemeralScratchRoot.empty()
          ? std::filesystem::temp_directory_path() / "aio-v"
          : std::move(ephemeralScratchRoot))
{
}

common::Result<std::filesystem::path> ManifestVerificationManualHandoffService::resolveCandidateRoot(
    const ManifestVerificationManualHandoff& handoff) const
{
    std::error_code error;
    std::filesystem::create_directories(manualHandoffRoot_, error);
    std::error_code rootReparseError;
    if (isReparsePoint(manualHandoffRoot_, rootReparseError) || rootReparseError) {
        return fail<std::filesystem::path>("manual_handoff.storage_containment_failed",
            "The manual-handoff storage root is not a safe physical directory");
    }
    const auto canonicalRoot = std::filesystem::weakly_canonical(manualHandoffRoot_, error);
    if (error) return fail<std::filesystem::path>("manual_handoff.storage_containment_failed",
        "The manual-handoff storage root could not be resolved");
    const std::filesystem::path storageKey(handoff.storageKey);
    if (storageKey.empty() || storageKey.is_absolute() || storageKey.has_root_name()
        || storageKey.has_root_directory() || storageKey.has_parent_path()
        || storageKey.filename() != storageKey || storageKey == "." || storageKey == "..") {
        return fail<std::filesystem::path>("manual_handoff.storage_containment_failed",
            "The manual-handoff storage key is not one safe path component");
    }
    const auto candidate = canonicalRoot / storageKey;
    std::error_code candidateReparseError;
    if (isReparsePoint(candidate, candidateReparseError)
        || (candidateReparseError && !isMissingFilesystemError(candidateReparseError))) {
        return fail<std::filesystem::path>("manual_handoff.storage_containment_failed",
            "The manual-handoff candidate directory is not a safe physical directory");
    }
    std::error_code existsError;
    const bool candidateExists = std::filesystem::exists(candidate, existsError);
    if (existsError && !isMissingFilesystemError(existsError)) {
        return fail<std::filesystem::path>("manual_handoff.storage_containment_failed",
            "The manual-handoff candidate directory could not be inspected safely");
    }
    // A missing direct child is a legitimate, typed health state. It has no
    // filesystem object whose canonical target could escape, while the
    // single-component check above and canonical root already prove lexical
    // containment. Requiring weakly_canonical() here made this state report
    // storage_containment_failed on Windows, so probeCandidateHealth() could
    // never project Missing and importAndReverify() could never reach its
    // documented workspace_unstable capture failure.
    if (!candidateExists) {
        return common::Result<std::filesystem::path>::ok(candidate);
    }
    const auto canonicalCandidate = std::filesystem::weakly_canonical(candidate, error);
    if (error || canonicalCandidate.parent_path() != canonicalRoot) {
        return fail<std::filesystem::path>("manual_handoff.storage_containment_failed",
            "The manual-handoff candidate directory failed containment verification");
    }
    return common::Result<std::filesystem::path>::ok(canonicalCandidate);
}

common::Result<ManifestVerificationManualHandoffCandidateProbe>
ManifestVerificationManualHandoffService::probeCandidateHealth(
    const ManifestVerificationManualHandoff& handoff) const
{
    const auto inspection = inspectCandidateEdits(handoff);
    if (!inspection.success()) {
        return common::Result<ManifestVerificationManualHandoffCandidateProbe>::failure(
            *inspection.error());
    }
    return common::Result<ManifestVerificationManualHandoffCandidateProbe>::ok(
        inspection.value().candidate);
}

common::Result<ManifestVerificationManualHandoffCandidateEditInspection>
ManifestVerificationManualHandoffService::inspectCandidateEdits(
    const ManifestVerificationManualHandoff& handoff) const
{
    ManifestVerificationManualHandoffCandidateEditInspection inspection;
    const auto root = resolveCandidateRoot(handoff);
    if (!root.success()) {
        inspection.candidate.health =
            ManifestVerificationManualHandoffCandidateHealth::StorageMismatch;
        return common::Result<ManifestVerificationManualHandoffCandidateEditInspection>::ok(
            std::move(inspection));
    }
    std::error_code existsError;
    const bool exists = std::filesystem::exists(root.value(), existsError);
    if (existsError || !exists) {
        inspection.candidate.health = ManifestVerificationManualHandoffCandidateHealth::Missing;
        return common::Result<ManifestVerificationManualHandoffCandidateEditInspection>::ok(
            std::move(inspection));
    }
    const auto snapshot = WorkspaceBaselineProvenance::capture(
        WorkspaceReadPolicy::secureDefaults(root.value()));
    if (!snapshot.success()) {
        inspection.candidate.health = ManifestVerificationManualHandoffCandidateHealth::Unreadable;
        return common::Result<ManifestVerificationManualHandoffCandidateEditInspection>::ok(
            std::move(inspection));
    }
    const bool unchanged = snapshot.value().fingerprint == handoff.originalSnapshot.fingerprint;
    inspection.candidate = {
        unchanged ? ManifestVerificationManualHandoffCandidateHealth::Unchanged
                  : ManifestVerificationManualHandoffCandidateHealth::Edited,
        snapshot.value().fingerprint};
    if (unchanged) {
        return common::Result<ManifestVerificationManualHandoffCandidateEditInspection>::ok(
            std::move(inspection));
    }

    std::unordered_map<std::string, WorkspaceBaselineEntry> originalByPath;
    for (const auto& file : handoff.originalSnapshot.files) {
        originalByPath.emplace(
            detail::normalizedPathKey(detail::pathFromUtf8(file.relativePath)), file);
    }
    std::unordered_map<std::string, WorkspaceBaselineEntry> liveByPath;
    for (const auto& file : snapshot.value().files) {
        liveByPath.emplace(
            detail::normalizedPathKey(detail::pathFromUtf8(file.relativePath)), file);
    }
    std::unordered_set<std::string> allowedPaths;
    for (const auto& allowed : handoff.allowedPaths) {
        allowedPaths.insert(
            detail::normalizedPathKey(detail::pathFromUtf8(allowed.relativePath)));
    }

    // A file created after materialization is never in allowedPaths: Create
    // proposals are already present in originalSnapshot before the user sees
    // the candidate. It therefore always belongs to the explicit reset bucket.
    for (const auto& [key, live] : liveByPath) {
        if (!originalByPath.contains(key)) inspection.resetRequiredPaths.push_back(live.relativePath);
    }
    for (const auto& [key, original] : originalByPath) {
        const auto live = liveByPath.find(key);
        if (live == liveByPath.end()) {
            // Delete is outside the manual-handoff MVP even for an otherwise
            // editable path, so the reset command may safely restore it.
            inspection.resetRequiredPaths.push_back(original.relativePath);
        } else if (live->second.sha256 != original.sha256) {
            if (allowedPaths.contains(key)) {
                inspection.editableChangedPaths.push_back(original.relativePath);
            } else {
                inspection.resetRequiredPaths.push_back(original.relativePath);
            }
        }
    }
    std::ranges::sort(inspection.editableChangedPaths);
    std::ranges::sort(inspection.resetRequiredPaths);
    inspection.resetRequiredPaths.erase(
        std::unique(inspection.resetRequiredPaths.begin(), inspection.resetRequiredPaths.end()),
        inspection.resetRequiredPaths.end());
    return common::Result<ManifestVerificationManualHandoffCandidateEditInspection>::ok(
        std::move(inspection));
}

common::Result<std::filesystem::path>
ManifestVerificationManualHandoffService::resolveCandidateRootForAnchor(
    const RunId& verificationAnchorRunId) const
{
    const auto handoff = manualHandoffs_.findHandoffByAnchorRun(verificationAnchorRunId);
    if (!handoff.success()) return fail<std::filesystem::path>(
        handoff.error()->code, handoff.error()->message);
    if (!handoff.value().has_value()) {
        return fail<std::filesystem::path>(
            "manual_handoff.not_found",
            "No manual handoff exists yet for this verification anchor Run");
    }
    return resolveCandidateRoot(*handoff.value());
}

namespace {

// QML-P4R.3: shared by materialize()'s two existing-handoff idempotent
// return points - a candidate whose directory is missing, unreadable, or
// fails storage containment must never be silently handed back as if it
// were still a usable path. Unchanged/Edited are both healthy enough to
// return: an edit in progress is exactly what a manual handoff is for.
bool candidateUnavailable(ManifestVerificationManualHandoffCandidateHealth health)
{
    return health == ManifestVerificationManualHandoffCandidateHealth::Missing
        || health == ManifestVerificationManualHandoffCandidateHealth::Unreadable
        || health == ManifestVerificationManualHandoffCandidateHealth::StorageMismatch;
}

// PC-4C: nudges the Outcome Brief reconciler about the anchor on EVERY exit of a command that can change its handoff or the
// resolution of it (best effort, never fails the command). It is declared BEFORE the command takes its locks, so it runs after they
// are released: the reconciler reads the same handoff state the command guards.
class OutcomeBriefNudge final {
public:
    explicit OutcomeBriefNudge(IOutcomeBriefTrigger* trigger) noexcept : trigger_(trigger) {}
    OutcomeBriefNudge(const OutcomeBriefNudge&) = delete;
    OutcomeBriefNudge& operator=(const OutcomeBriefNudge&) = delete;
    ~OutcomeBriefNudge()
    {
        if (trigger_ == nullptr) return;
        for (const auto& anchorRunId : anchorRunIds_) trigger_->runMayHaveChanged(anchorRunId);
    }
    void addAnchor(RunId anchorRunId)
    {
        if (!anchorRunId.empty() && std::find(anchorRunIds_.begin(), anchorRunIds_.end(), anchorRunId) == anchorRunIds_.end()) {
            anchorRunIds_.push_back(std::move(anchorRunId));
        }
    }

private:
    IOutcomeBriefTrigger* trigger_;
    std::vector<RunId> anchorRunIds_;
};

// Moves the anchors of the handoffs that the command just deleted (collected under importMutex_) into its nudge. Declared AFTER the
// command took importMutex_, so it runs while the lock is still held; the nudge itself, declared before the lock, fires after it.
class DrainDiscardedAnchors final {
public:
    DrainDiscardedAnchors(std::vector<RunId>& discarded, OutcomeBriefNudge& nudge) noexcept : discarded_(discarded), nudge_(nudge) {}
    DrainDiscardedAnchors(const DrainDiscardedAnchors&) = delete;
    DrainDiscardedAnchors& operator=(const DrainDiscardedAnchors&) = delete;
    ~DrainDiscardedAnchors()
    {
        for (auto& anchor : discarded_) nudge_.addAnchor(std::move(anchor));
        discarded_.clear();
    }

private:
    std::vector<RunId>& discarded_;
    OutcomeBriefNudge& nudge_;
};

} // namespace

common::Result<ManifestVerificationManualHandoffMaterializeResult>
ManifestVerificationManualHandoffService::materialize(
    const RunId& verificationAnchorRunId, std::stop_token stopToken)
{
    OutcomeBriefNudge nudge(outcomeBriefTrigger_);
    nudge.addAnchor(verificationAnchorRunId);
    const auto anchor = runs_.findRun(verificationAnchorRunId);
    if (!anchor.success()) return fail<ManifestVerificationManualHandoffMaterializeResult>(
        anchor.error()->code, anchor.error()->message);
    if (!anchor.value().has_value()) return fail<ManifestVerificationManualHandoffMaterializeResult>(
        "manual_handoff.source_missing", "The verification anchor Run does not exist");

    const auto existing = manualHandoffs_.findHandoffByAnchorRun(verificationAnchorRunId);
    if (!existing.success()) return fail<ManifestVerificationManualHandoffMaterializeResult>(
        existing.error()->code, existing.error()->message);
    // A simple, same-Run handoff keeps the original idempotent fast path.
    // A mixed-run handoff created by the pre-hardening P4R.2 code must not
    // bypass the new cross-aggregate and lineage checks merely because its
    // immutable row already exists; that case is revalidated below before it
    // can be returned.
    if (existing.value().has_value()
        && anchor.value()->workflowId != kManifestVerificationContinuationWorkflowId) {
        // QML-P4R.3: validate existence/readability/containment on every
        // load - never hand back a path whose directory no longer safely
        // exists just because the immutable handoff row does. A changed
        // fingerprint (Edited) is never itself a reason to refuse here.
        const auto probe = probeCandidateHealth(*existing.value());
        if (!probe.success()) return fail<ManifestVerificationManualHandoffMaterializeResult>(
            probe.error()->code, probe.error()->message);
        if (candidateUnavailable(probe.value().health)) {
            return fail<ManifestVerificationManualHandoffMaterializeResult>(
                "manual_handoff.candidate_unavailable",
                "The existing handoff's candidate directory is missing, unreadable, or fails "
                "storage containment");
        }
        const auto root = resolveCandidateRoot(*existing.value());
        if (!root.success()) return fail<ManifestVerificationManualHandoffMaterializeResult>(
            root.error()->code, root.error()->message);
        return common::Result<ManifestVerificationManualHandoffMaterializeResult>::ok(
            {*existing.value(), root.value()});
    }

    // QML-P4R.2: resolve which Run actually owns the frozen Manifest/tasks
    // this handoff must verify against - see resolveOriginManifestRun's own
    // doc comment. QML-P4R.5: extracted into a shared helper so
    // evaluateCreateEligibility() can reuse the identical rule; behavior
    // here is unchanged.
    const auto originResolution = resolveOriginManifestRun(*anchor.value());
    if (!originResolution.success()) return fail<ManifestVerificationManualHandoffMaterializeResult>(
        originResolution.error()->code, originResolution.error()->message);
    const RunId originManifestRunId = originResolution.value().originManifestRunId;
    const std::optional<ManifestVerificationContinuationInput> continuationInput =
        originResolution.value().continuationInput;

    const auto ledger = workLedger_.findEntry(verificationAnchorRunId);
    if (!ledger.success()) return fail<ManifestVerificationManualHandoffMaterializeResult>(
        ledger.error()->code, ledger.error()->message);

    // Admission: the simple case reuses the same durable checkpoint
    // machinery paid continuation preparation shares - eligibility is a
    // property of the origin Run's own failure, not of which repair
    // mechanism reads it. The mixed case's anchor is itself a
    // continuation-workflow Run, which admission_ always denies with
    // not_manifest_workflow (MW-P5's own no-chaining rule, correctly
    // unchanged) - so it is evaluated directly against the anchor's own
    // ledger through the same shared content-repairable-failure check
    // instead, never a durable checkpoint (this handoff's own row is
    // already this call's idempotency boundary).
    if (verificationAnchorRunId == originManifestRunId) {
        const auto admitted = admission_.admitOrEvaluate(verificationAnchorRunId);
        if (!admitted.success()) return fail<ManifestVerificationManualHandoffMaterializeResult>(
            admitted.error()->code, admitted.error()->message);
        if (admitted.value().status != ManifestVerificationContinuationStatus::Admitted)
            return fail<ManifestVerificationManualHandoffMaterializeResult>(
                admitted.value().denialCode, admitted.value().denialMessage);
    } else {
        if (anchor.value()->status != RunStatus::Failed) {
            return fail<ManifestVerificationManualHandoffMaterializeResult>(
                "verification_continuation.source_run_not_failed",
                "Manual handoff requires a terminal Failed anchor Run");
        }
        if (!ledger.value().has_value()) {
            return fail<ManifestVerificationManualHandoffMaterializeResult>(
                "verification_continuation.missing_work_ledger_entry",
                "The anchor Run has no durable Work Ledger entry");
        }
        // QML-P4R.2 hardening: the ledger lookup is keyed by
        // verificationAnchorRunId, but the row's own recorded fields are not
        // re-derived from that key - a corrupted/mis-attributed entry must
        // fail closed rather than being admitted on the strength of the
        // lookup key alone.
        if (ledger.value()->runId != verificationAnchorRunId
            || ledger.value()->projectId != anchor.value()->projectId
            || ledger.value()->terminalStatus != RunStatus::Failed) {
            return fail<ManifestVerificationManualHandoffMaterializeResult>(
                "manual_handoff.origin_identity_conflict",
                "The anchor Run's own Work Ledger entry does not match its identity or is not a "
                "terminal failure");
        }
        const auto denial = determineContentRepairableFailureDenial(*ledger.value());
        if (!denial.eligible()) {
            return fail<ManifestVerificationManualHandoffMaterializeResult>(
                denial.code, denial.message);
        }
    }

    const auto manifest = manifests_.findForRun(originManifestRunId);
    if (!manifest.success()) return fail<ManifestVerificationManualHandoffMaterializeResult>(
        manifest.error()->code, manifest.error()->message);
    if (!manifest.value().has_value() || !ledger.value().has_value()
        || !ledger.value()->verification.has_value())
        return fail<ManifestVerificationManualHandoffMaterializeResult>(
            "manual_handoff.source_corrupt",
            "The origin Manifest or anchor verification identity is missing");
    const auto& durable = *ledger.value()->verification;

    // QML-P4R.2 hardening: the origin and the anchor must be proven to be
    // looking at the SAME physical workspace, not merely the same project id
    // - the continuation input's own workspaceIdentity was captured at the
    // origin's prepare()-time and never re-derived here.
    if (continuationInput.has_value()
        && (continuationInput->sourceWorkspaceIdentity != durable.workspaceIdentity
            || continuationInput->sourceWorkspaceIdentity != anchor.value()->projectId
            || durable.workspaceIdentity != anchor.value()->projectId
            || continuationInput->baselineFingerprint != durable.baselineFingerprint
            || durable.baselineFingerprint != ledger.value()->baselineFingerprint)) {
        return fail<ManifestVerificationManualHandoffMaterializeResult>(
            "manual_handoff.origin_identity_conflict",
            "The continuation input, anchor verification, and Work Ledger workspace/baseline "
            "identities do not match the anchor's own project");
    }

    const auto project = projects_.findById(anchor.value()->projectId);
    if (!project.success()) return fail<ManifestVerificationManualHandoffMaterializeResult>(
        project.error()->code, project.error()->message);
    if (!project.value().has_value()) return fail<ManifestVerificationManualHandoffMaterializeResult>(
        "manual_handoff.source_project_missing", "The source project does not exist");
    const auto workspaceRoot = anchor.value()->workspacePath.value_or(project.value()->rootPath);
    // The validated root of the TEMPORARY verification copies (the durable candidate below stays under manualHandoffRoot_): a
    // root that is not usable is refused here, before anything is baselined, copied or run.
    const auto ephemeralRoot = resolveVerificationScratchRoot(scratchProvider_, ephemeralScratchRoot_, workspaceRoot);
    if (!ephemeralRoot.success()) return fail<ManifestVerificationManualHandoffMaterializeResult>(
        ephemeralRoot.error()->code, ephemeralRoot.error()->message);
    if (continuationInput.has_value()) {
        const auto originRun = runs_.findRun(originManifestRunId);
        if (!originRun.success()) return fail<ManifestVerificationManualHandoffMaterializeResult>(
            originRun.error()->code, originRun.error()->message);
        if (!originRun.value().has_value()) return fail<ManifestVerificationManualHandoffMaterializeResult>(
            "manual_handoff.origin_identity_conflict", "The origin Run no longer exists");
        const auto originWorkspaceRoot = originRun.value()->workspacePath.value_or(
            project.value()->rootPath);
        std::error_code anchorRootError;
        std::error_code originRootError;
        const auto canonicalAnchorRoot = std::filesystem::weakly_canonical(
            workspaceRoot, anchorRootError);
        const auto canonicalOriginRoot = std::filesystem::weakly_canonical(
            originWorkspaceRoot, originRootError);
        if (anchorRootError || originRootError || canonicalAnchorRoot != canonicalOriginRoot) {
            return fail<ManifestVerificationManualHandoffMaterializeResult>(
                "manual_handoff.origin_identity_conflict",
                "The origin and anchor Runs do not resolve to the same project workspace");
        }
    }

    // Revalidate a pre-hardening immutable mixed-run handoff before taking
    // the idempotent return path. The row is never rewritten: it is either
    // proven to be the exact handoff current durable identities imply, or it
    // fails closed as incompatible/corrupt legacy state.
    if (existing.value().has_value()) {
        const auto& handoff = *existing.value();
        if (!continuationInput.has_value()
            || handoff.originManifestRunId != originManifestRunId
            || handoff.verificationAnchorRunId != verificationAnchorRunId
            || handoff.sourceProjectId != anchor.value()->projectId
            || handoff.workspaceIdentity != durable.workspaceIdentity
            || handoff.baselineFingerprint != durable.baselineFingerprint
            || handoff.sourceOverlayFingerprint != durable.overlayFingerprint
            || handoff.frozenMembers.size() != durable.orderedProposalIds.size()
            || durable.orderedProposalContentHashes.size() != durable.orderedProposalIds.size()
            || continuationInput->orderedProposalIds.size() != handoff.frozenMembers.size()
            || continuationInput->orderedProposalContentHashes.size()
                != continuationInput->orderedProposalIds.size()) {
            return fail<ManifestVerificationManualHandoffMaterializeResult>(
                "manual_handoff.origin_identity_conflict",
                "The existing mixed-run handoff does not match its current durable anchor identity");
        }

        std::unordered_set<ChangeSetId> existingRoots;
        for (std::size_t i = 0; i < handoff.frozenMembers.size(); ++i) {
            const auto& frozen = handoff.frozenMembers[i];
            const auto& durableId = durable.orderedProposalIds[i];
            const auto proposal = proposals_.findChangeSet(durableId);
            const auto root = resolveLineageRoot(proposals_, durableId);
            if (!proposal.success() || !root.success()
                || frozen.proposalId != durableId
                || frozen.owningRunId != proposal.value().runId
                || frozen.contentHash != durable.orderedProposalContentHashes[i]
                || frozen.lineageRootProposalId != root.value()
                || !existingRoots.insert(root.value()).second) {
                return fail<ManifestVerificationManualHandoffMaterializeResult>(
                    "manual_handoff.lineage_set_mismatch",
                    "The existing mixed-run handoff does not exactly match the anchor's ordered "
                    "proposal identity and lineage");
            }
            const auto inputRoot = resolveLineageRoot(
                proposals_, continuationInput->orderedProposalIds[i]);
            if (!inputRoot.success() || inputRoot.value() != root.value()) {
                return fail<ManifestVerificationManualHandoffMaterializeResult>(
                    "manual_handoff.lineage_set_mismatch",
                    "The existing mixed-run handoff is not an ordered one-to-one representation "
                    "of the continuation input");
            }
            const bool unchangedOriginMember = proposal.value().runId == originManifestRunId;
            if (unchangedOriginMember
                && (continuationInput->orderedProposalIds[i] != durableId
                    || continuationInput->orderedProposalContentHashes[i]
                        != durable.orderedProposalContentHashes[i])) {
                return fail<ManifestVerificationManualHandoffMaterializeResult>(
                    "manual_handoff.source_candidate_mismatch",
                    "An unchanged origin-owned member does not match the continuation input's "
                    "ordered identity and content hash");
            }
        }
        // QML-P4R.3: same existence/readability/containment validation as
        // the simple-case fast path above - a revalidated mixed-run handoff
        // is still not returned if its candidate directory itself is gone.
        const auto probe = probeCandidateHealth(handoff);
        if (!probe.success()) return fail<ManifestVerificationManualHandoffMaterializeResult>(
            probe.error()->code, probe.error()->message);
        if (candidateUnavailable(probe.value().health)) {
            return fail<ManifestVerificationManualHandoffMaterializeResult>(
                "manual_handoff.candidate_unavailable",
                "The existing handoff's candidate directory is missing, unreadable, or fails "
                "storage containment");
        }
        const auto root = resolveCandidateRoot(handoff);
        if (!root.success()) return fail<ManifestVerificationManualHandoffMaterializeResult>(
            root.error()->code, root.error()->message);
        return common::Result<ManifestVerificationManualHandoffMaterializeResult>::ok(
            {handoff, root.value()});
    }

    const auto policy = WorkspaceReadPolicy::secureDefaults(workspaceRoot);
    const auto currentBaseline = WorkspaceBaselineProvenance::capture(policy);
    if (!currentBaseline.success()) return fail<ManifestVerificationManualHandoffMaterializeResult>(
        currentBaseline.error()->code, currentBaseline.error()->message);
    std::optional<ManifestVerificationBaselineNormalization> baselineNormalization;
    if (currentBaseline.value().fingerprint != durable.baselineFingerprint) {
        if (continuationInput.has_value()) {
            // QML-P4R.2 hardening, explicit rather than incidental: legacy-
            // baseline normalization (ManifestVerificationLegacyBaselineNormalizationService)
            // is scoped to a single sourceRun's own proposal set - see its
            // own eligibility contract, which requires every proposal to
            // belong to the passed sourceRunId. A mixed anchor's frozen set
            // mixes origin- and anchor-owned members by design, so it can
            // never satisfy that contract, and any legitimate legacy drift
            // was already proven (or found ineligible) once, against the
            // ORIGIN Run, at prepare()-time - that proof is already baked
            // into durable.baselineFingerprint for a mixed anchor. Re-running
            // normalization here would either spuriously reject on the first
            // origin-owned member or, on a future refactor, risk minting a
            // second, redundant proof for drift already covered upstream.
            // Any mismatch reaching this point is real, unproven drift since
            // the anchor's own verification and must fail closed - never
            // silently re-normalized.
            return fail<ManifestVerificationManualHandoffMaterializeResult>(
                "manual_handoff.baseline_drift",
                "The workspace no longer matches the anchor's own verification baseline; implicit "
                "rebase is forbidden");
        }
        const auto normalized = baselineNormalization_.normalizeOrEvaluate(
            verificationAnchorRunId, durable.baselineFingerprint,
            currentBaseline.value(), durable.orderedProposalIds);
        if (!normalized.success()) return fail<ManifestVerificationManualHandoffMaterializeResult>(
            normalized.error()->code, normalized.error()->message);
        if (!normalized.value().has_value()) {
            return fail<ManifestVerificationManualHandoffMaterializeResult>(
                "manual_handoff.baseline_drift",
                "The workspace no longer matches the source Run baseline; implicit rebase is forbidden");
        }
        baselineNormalization = normalized.value();
    }

    // Recheck source membership before materialization. A member must
    // belong to EITHER the origin Manifest Run (an unchanged reference) or
    // the anchor itself (the anchor's own successor, mixed-run case only) -
    // never any other Run. An origin-owned member in the mixed case is
    // additionally cross-checked against the exact candidate set
    // QML-P4R.2's own continuation input already froze for this anchor at
    // prepare() time - never merely because it happens to point at the
    // origin Run - the same discipline AG-16's own continuation eligibility
    // proof (evaluateContinuationEligibility) already applies at Apply time.
    std::unordered_set<ChangeSetId> originCandidateIds;
    std::unordered_map<ChangeSetId, std::string> inputContentHashById;
    if (continuationInput.has_value()) {
        originCandidateIds.insert(
            continuationInput->orderedProposalIds.begin(), continuationInput->orderedProposalIds.end());
        for (std::size_t i = 0; i < continuationInput->orderedProposalIds.size(); ++i) {
            inputContentHashById.emplace(continuationInput->orderedProposalIds[i],
                continuationInput->orderedProposalContentHashes.at(i));
        }
    }
    std::unordered_map<ChangeSetId, ProposedChangeSet> membersById;
    for (std::size_t i = 0; i < durable.orderedProposalIds.size(); ++i) {
        const auto& id = durable.orderedProposalIds[i];
        const auto proposal = proposals_.findChangeSet(id);
        if (!proposal.success() || proposal.value().projectId != anchor.value()->projectId
            || proposal.value().state != ChangeSetState::ReadyForReview) {
            return fail<ManifestVerificationManualHandoffMaterializeResult>(
                "manual_handoff.source_proposal_unavailable",
                "An exact source proposal is missing, changed, or no longer reviewable");
        }
        const bool ownedByAnchor = proposal.value().runId == verificationAnchorRunId;
        const bool ownedByOrigin = proposal.value().runId == originManifestRunId
            && (!continuationInput.has_value() || originCandidateIds.contains(id));
        if (!ownedByAnchor && !ownedByOrigin) {
            return fail<ManifestVerificationManualHandoffMaterializeResult>(
                "manual_handoff.source_proposal_unavailable",
                "An exact source proposal is missing, changed, or no longer reviewable");
        }
        // QML-P4R.2 hardening: an unmodified origin-owned member is not
        // merely required to still carry the right id - its exact bytes, as
        // recorded when the continuation input itself froze this id at
        // prepare() time, must still match what the anchor's own final
        // verification recorded for that same id. A ledger/proposal-store
        // inconsistency that silently swapped in different content under
        // the same id must fail closed here, not slip through as "still
        // origin-owned."
        if (ownedByOrigin && continuationInput.has_value()) {
            const auto inputHash = inputContentHashById.find(id);
            if (inputHash == inputContentHashById.end()
                || inputHash->second != durable.orderedProposalContentHashes.at(i)) {
                return fail<ManifestVerificationManualHandoffMaterializeResult>(
                    "manual_handoff.source_candidate_mismatch",
                    "An unchanged origin-owned member's content hash does not match the "
                    "continuation input's own frozen record");
            }
        }
        membersById.emplace(id, proposal.value());
    }

    BuildManifestProposalOverlayRequest baselineRequest;
    baselineRequest.workspaceRoot = workspaceRoot;
    baselineRequest.trustedScratchRoot = ephemeralRoot.value();
    baselineRequest.workspaceIdentity = durable.workspaceIdentity;
    baselineRequest.baselinePolicy = policy;
    baselineRequest.baselineSnapshot = currentBaseline.value();
    auto baselineOverlay = ManifestProposalOverlay::build(baselineRequest, proposals_);
    if (!baselineOverlay.success()) return fail<ManifestVerificationManualHandoffMaterializeResult>(
        baselineOverlay.error()->code, baselineOverlay.error()->message);
    struct EphemeralCleanup {
        ManifestProposalOverlayDescriptor descriptor;
        ~EphemeralCleanup() { static_cast<void>(ManifestProposalOverlay::cleanup(descriptor)); }
    } baselineCleanup {baselineOverlay.value()};

    // Materialized into the durable, Backend-owned manual-handoff root -
    // never cleaned up on success, unlike every other overlay build in this
    // function - see this class's own doc comment.
    auto candidateRequest = baselineRequest;
    candidateRequest.trustedScratchRoot = manualHandoffRoot_;
    candidateRequest.orderedProposalIds = durable.orderedProposalIds;
    auto candidateOverlay = ManifestProposalOverlay::build(candidateRequest, proposals_);
    if (!candidateOverlay.success()) return fail<ManifestVerificationManualHandoffMaterializeResult>(
        candidateOverlay.error()->code, candidateOverlay.error()->message);
    CandidateOverlayCleanup candidateCleanup {candidateOverlay.value(), false};
    if (candidateOverlay.value().orderedProposalContentHashes
            != durable.orderedProposalContentHashes) {
        return fail<ManifestVerificationManualHandoffMaterializeResult>(
            "manual_handoff.source_candidate_mismatch",
            "The reconstructed source proposal set does not match its durable verification identity");
    }
    if (!baselineNormalization.has_value()
        && candidateOverlay.value().overlayFingerprint != durable.overlayFingerprint) {
        return fail<ManifestVerificationManualHandoffMaterializeResult>(
            "manual_handoff.source_candidate_mismatch",
            "The reconstructed source proposal set does not match its durable verification identity");
    }

    // QML-P4R.6: the durable candidate above lives under manualHandoffRoot_
    // by design - it must persist past this call for the user to open and
    // edit, so it can never be the ephemeral scratch root. manualHandoffRoot_
    // is necessarily database-adjacent (production: the per-user AppData
    // directory) and can be long enough that, combined with a project's own
    // generated CMake build-directory name, the durable candidate's own path
    // trips overlayPathBudgetCheck's Windows path-budget preflight (see
    // ManifestProposalSetVerifier.cpp) even though the EXACT SAME content
    // verifies cleanly under a short root - confirmed by the QML-P4R.6
    // retained-Run canary investigation. Verification therefore never runs
    // against the durable directory itself: an execution-only overlay is
    // built from the IDENTICAL trusted inputs (same workspaceRoot/
    // baselinePolicy/baselineSnapshot/orderedProposalIds already captured in
    // candidateRequest) under the short ephemeralScratchRoot_ instead. This
    // produces byte-identical content to the durable candidate by
    // construction - proven, not assumed, by the fingerprint/content-hash
    // equality check below - so this is what "the durable candidate's exact
    // content, verified from a short scratch copy" means here: a second
    // deterministic build from the same trusted source, never a read of the
    // durable directory's own raw bytes (which the user may start editing as
    // soon as this call returns, and which this function must never move,
    // rename, or replace).
    const auto durablePreVerifySnapshot = WorkspaceBaselineProvenance::capture(
        WorkspaceReadPolicy::secureDefaults(candidateOverlay.value().root));
    if (!durablePreVerifySnapshot.success()) {
        return fail<ManifestVerificationManualHandoffMaterializeResult>(
            "manual_handoff.workspace_unstable",
            "The materialized candidate directory could not be captured before verification");
    }
    auto executionRequest = candidateRequest;
    executionRequest.trustedScratchRoot = ephemeralRoot.value();
    auto executionOverlay = ManifestProposalOverlay::build(executionRequest, proposals_);
    if (!executionOverlay.success()) return fail<ManifestVerificationManualHandoffMaterializeResult>(
        executionOverlay.error()->code, executionOverlay.error()->message);
    EphemeralCleanup executionCleanup {executionOverlay.value()};
    if (executionOverlay.value().overlayFingerprint != candidateOverlay.value().overlayFingerprint
        || executionOverlay.value().orderedProposalContentHashes
            != candidateOverlay.value().orderedProposalContentHashes) {
        return fail<ManifestVerificationManualHandoffMaterializeResult>(
            "manual_handoff.workspace_unstable",
            "The short-root verification copy did not match the durable candidate's own content");
    }

    const auto baselineVerification = ManifestProposalSetVerifier::verify(
        {baselineOverlay.value(), manifest.value()->tasks}, proposals_, commandRunner_, stopToken);
    if (!baselineVerification.success()) return fail<ManifestVerificationManualHandoffMaterializeResult>(
        baselineVerification.error()->code, baselineVerification.error()->message);
    ManifestProposalSetVerifierRequest candidateVerifierRequest {
        executionOverlay.value(), manifest.value()->tasks};
    if (usesBaselineAwareQmlLintPolicy(durable)) {
        candidateVerifierRequest.baselineOverlay = &baselineOverlay.value();
        candidateVerifierRequest.baselineVerification = &baselineVerification.value();
    }
    const auto candidateVerification = ManifestProposalSetVerifier::verify(
        candidateVerifierRequest, proposals_, commandRunner_, stopToken);
    if (!candidateVerification.success()) return fail<ManifestVerificationManualHandoffMaterializeResult>(
        candidateVerification.error()->code, candidateVerification.error()->message);

    // Confirm the durable candidate was not touched while verification ran
    // in the short-root copy above (which can take tens of seconds for a
    // real CMake build) - the durable directory itself is never read FOR
    // verification, only re-hashed here to prove it is still exactly what
    // materialize() itself just produced.
    const auto durablePostVerifySnapshot = WorkspaceBaselineProvenance::capture(
        WorkspaceReadPolicy::secureDefaults(candidateOverlay.value().root));
    if (!durablePostVerifySnapshot.success()
        || durablePostVerifySnapshot.value().fingerprint != durablePreVerifySnapshot.value().fingerprint) {
        return fail<ManifestVerificationManualHandoffMaterializeResult>(
            "manual_handoff.workspace_unstable",
            "The materialized candidate directory changed while verification was running");
    }

    // The simple case reuses the same durable attribution checkpoint the
    // paid continuation-preparation path shares (attribution is a property
    // of the origin Run's own failure) - unchanged from before this
    // package. The mixed case's anchor is the paid continuation child
    // itself: attributeOrEvaluate would require an admission checkpoint AND
    // a frozen Manifest keyed by the ANCHOR, neither of which exist for a
    // Run whose Manifest it does not itself own - attributeOrEvaluateForAnchor
    // reads those origin-owned pieces from originManifestRunId instead
    // while still attributing the anchor's own Work Ledger evidence.
    // A verification that was refused for the path budget is not a verdict on the code: say so instead of letting attribution
    // report an unmapped diagnostic.
    if (verificationRefusedForPathBudget(baselineVerification.value())
        || verificationRefusedForPathBudget(candidateVerification.value())) {
        return fail<ManifestVerificationManualHandoffMaterializeResult>(
            "manual_handoff.verification_path_too_long",
            "Verification could not run: the temporary verification copy of the workspace would exceed the Windows path "
            "budget of the build tools. Use a shorter temporary-directory location (TEMP/TMP) or a shorter project location.");
    }
    const auto attributed = verificationAnchorRunId == originManifestRunId
        ? attribution_.attributeOrEvaluate(
              verificationAnchorRunId, baselineVerification.value(), candidateVerification.value())
        : attribution_.attributeOrEvaluateForAnchor(verificationAnchorRunId, originManifestRunId,
              baselineVerification.value(), candidateVerification.value());
    if (!attributed.success()) return fail<ManifestVerificationManualHandoffMaterializeResult>(
        attributed.error()->code, attributed.error()->message);
    if (attributed.value().status != ManifestVerificationAttributionStatus::Attributed) {
        return fail<ManifestVerificationManualHandoffMaterializeResult>(
            attributed.value().blockCode, attributed.value().blockMessage);
    }

    // Restricted to a REMAINING owner finding's own Create/Modify target -
    // never every path the frozen source proposal set happens to touch, and
    // never a Rename target (see this class's own doc comment).
    std::vector<ManifestVerificationManualHandoffAllowedPath> allowedPaths;
    std::unordered_set<std::string> seenPaths;
    std::unordered_map<ChangeSetId, std::vector<ProposedFileChange>> changesByProposal;
    for (const auto& finding : attributed.value().findings) {
        if (!isOwnerFinding(finding) || !finding.ownerProposalId.has_value()
            || !finding.ownerTaskId.has_value()) continue;
        if (seenPaths.contains(finding.relativePath)) continue;
        auto changesIt = changesByProposal.find(*finding.ownerProposalId);
        if (changesIt == changesByProposal.end()) {
            const auto changes = proposals_.listChanges(*finding.ownerProposalId);
            if (!changes.success()) continue;
            changesIt = changesByProposal.emplace(
                *finding.ownerProposalId, changes.value()).first;
        }
        // finding.relativePath is already normalized via
        // detail::normalizedPathKey(detail::pathFromUtf8(...)) - see
        // ManifestVerificationAttributionService.cpp's normalizedFindingPath.
        // A proposal's own stored ProposedFileChange::relativePath is NOT
        // (it retains its original submitted case/separators), so this side
        // must apply the exact same normalization for a legitimate match -
        // the same "both sides must apply this exact function" contract
        // FilesystemPathUtils.hpp's own doc comment states.
        const auto matchedChange = std::find_if(
            changesIt->second.begin(), changesIt->second.end(),
            [&](const ProposedFileChange& change) {
                return detail::normalizedPathKey(change.relativePath) == finding.relativePath
                    && (change.operation == FileChangeOperation::Create
                        || change.operation == FileChangeOperation::Modify);
            });
        if (matchedChange == changesIt->second.end()) continue;
        seenPaths.insert(finding.relativePath);
        // Stores the proposal's own real, as-submitted relativePath (not
        // finding.relativePath, which is lowercase-normalized evidence-
        // matching output) - this is what must match the real materialized
        // candidate directory's own on-disk file names later.
        allowedPaths.push_back({matchedChange->relativePath.generic_string(),
            *finding.ownerProposalId, *finding.ownerTaskId});
    }
    if (allowedPaths.empty()) {
        return fail<ManifestVerificationManualHandoffMaterializeResult>(
            "manual_handoff.no_editable_findings",
            "No remaining finding names a Create/Modify target eligible for manual handoff");
    }

    // Defence against a concurrent writer touching the just-materialized
    // candidate directory mid-freeze: two consecutive captures must agree
    // before this handoff is ever considered durable.
    const auto candidatePolicy = WorkspaceReadPolicy::secureDefaults(candidateOverlay.value().root);
    const auto firstSnapshot = WorkspaceBaselineProvenance::capture(candidatePolicy);
    const auto secondSnapshot = WorkspaceBaselineProvenance::capture(candidatePolicy);
    if (!firstSnapshot.success() || !secondSnapshot.success()) {
        return fail<ManifestVerificationManualHandoffMaterializeResult>(
            "manual_handoff.workspace_unstable",
            "The materialized candidate directory could not be captured");
    }
    if (firstSnapshot.value().fingerprint != secondSnapshot.value().fingerprint) {
        return fail<ManifestVerificationManualHandoffMaterializeResult>(
            "manual_handoff.workspace_unstable",
            "The materialized candidate directory changed while being frozen");
    }

    std::vector<ManifestVerificationManualHandoffFrozenMember> frozenMembers;
    frozenMembers.reserve(durable.orderedProposalIds.size());
    std::unordered_set<ChangeSetId> seenLineageRoots;
    std::vector<ChangeSetId> frozenLineageRoots;
    frozenLineageRoots.reserve(durable.orderedProposalIds.size());
    for (std::size_t i = 0; i < durable.orderedProposalIds.size(); ++i) {
        const auto& id = durable.orderedProposalIds[i];
        const auto& memberProposal = membersById.at(id);
        const auto lineageRoot = resolveLineageRoot(proposals_, id);
        if (!lineageRoot.success()) return fail<ManifestVerificationManualHandoffMaterializeResult>(
            lineageRoot.error()->code, lineageRoot.error()->message);
        if (!seenLineageRoots.insert(lineageRoot.value()).second) {
            return fail<ManifestVerificationManualHandoffMaterializeResult>(
                "manual_handoff.duplicate_lineage_root",
                "Two frozen members share the same lineage root - the exact source set contains "
                "two versions of the same proposal");
        }
        frozenMembers.push_back({
            id, memberProposal.runId, durable.orderedProposalContentHashes.at(i),
            lineageRoot.value()});
        frozenLineageRoots.push_back(lineageRoot.value());
    }

    // QML-P4R.2 hardening: the per-member ownership recheck above only
    // proves each individual durable member belongs to either the origin or
    // the anchor - it never proves the durable ordered sequence as a whole
    // is the exact sequence the continuation input itself froze. A corrupted
    // ledger could otherwise drop an input member outright, or mix in an anchor-owned
    // proposal unrelated to any input member's own lineage. Every input
    // member's lineage root must appear exactly once among the frozen
    // members (as itself if unmodified, or as its own genuine successor),
    // and no frozen member may carry a lineage root the input never froze.
    if (continuationInput.has_value()) {
        std::unordered_set<ChangeSetId> seenInputLineageRoots;
        std::vector<ChangeSetId> inputLineageRoots;
        inputLineageRoots.reserve(continuationInput->orderedProposalIds.size());
        for (const auto& id : continuationInput->orderedProposalIds) {
            const auto inputRoot = resolveLineageRoot(proposals_, id);
            if (!inputRoot.success()) return fail<ManifestVerificationManualHandoffMaterializeResult>(
                inputRoot.error()->code, inputRoot.error()->message);
            if (!seenInputLineageRoots.insert(inputRoot.value()).second) {
                return fail<ManifestVerificationManualHandoffMaterializeResult>(
                    "manual_handoff.duplicate_lineage_root",
                    "The continuation input's own frozen source set contains two versions of the "
                    "same proposal");
            }
            inputLineageRoots.push_back(inputRoot.value());
        }
        if (inputLineageRoots != frozenLineageRoots) {
            return fail<ManifestVerificationManualHandoffMaterializeResult>(
                "manual_handoff.lineage_set_mismatch",
                "The anchor's ordered frozen lineage roots do not match the continuation input's "
                "own ordered source identity - missing, extra, reordered, or substituted members "
                "are not permitted");
        }
    }

    ManifestVerificationManualHandoff handoff;
    handoff.id = "manual-handoff:" + verificationAnchorRunId;
    handoff.originManifestRunId = originManifestRunId;
    handoff.verificationAnchorRunId = verificationAnchorRunId;
    handoff.sourceProjectId = anchor.value()->projectId;
    handoff.storageKey = candidateOverlay.value().root.filename().string();
    handoff.workspaceIdentity = durable.workspaceIdentity;
    handoff.baselineFingerprint = currentBaseline.value().fingerprint;
    handoff.sourceOverlayFingerprint = candidateOverlay.value().overlayFingerprint;
    handoff.frozenMembers = std::move(frozenMembers);
    handoff.allowedPaths = std::move(allowedPaths);
    handoff.originalSnapshot = firstSnapshot.value();
    handoff.createdAt = std::chrono::system_clock::now();

    // QML-P4R.3.5: the capacity check and the insert it protects are one
    // locked segment, serialized against a concurrent materialize()'s own
    // capacity check (and against importAndReverify()/discardHandoff())
    // through the same importMutex_ - never the whole function; see this
    // method's own header doc comment for why. Deliberately scoped to
    // this block alone (not the rest of the function) so importLock is
    // released BEFORE the "already_exists" race-recovery branch below
    // ever recurses into materialize() again - that recursive call would
    // otherwise self-deadlock on the same non-recursive mutex.
    common::Result<ManifestVerificationManualHandoff> insertOutcome = [&] {
        std::lock_guard importLock(importMutex_);
        // PC-4C: the handoffs that this capacity check evicts lose their anchor's "waiting for the manual resolution": their anchors are
        // nudged too (after importLock is released, by `nudge`).
        DrainDiscardedAnchors drainEvicted(discardedAnchorsLocked_, nudge);
        const auto capacity = ensureRetentionCapacityForNewHandoffLocked();
        if (!capacity.success()) return common::Result<ManifestVerificationManualHandoff>::failure(
            *capacity.error());
        const auto inserted = manualHandoffs_.insertHandoff(handoff);
        if (!inserted.success()) return common::Result<ManifestVerificationManualHandoff>::failure(
            *inserted.error());
        return common::Result<ManifestVerificationManualHandoff>::ok(handoff);
    }();
    if (!insertOutcome.success()) {
        if (insertOutcome.error()->code == "manifest_verification_manual_handoff.already_exists") {
            const auto raced = manualHandoffs_.findHandoffByAnchorRun(verificationAnchorRunId);
            if (raced.success() && raced.value().has_value()) {
                // Re-enter through the ordinary existing-row path so a
                // concurrently inserted mixed-run handoff receives the same
                // identity/lineage validation as one observed at method
                // entry - importMutex_ is already released by this point.
                return materialize(verificationAnchorRunId, stopToken);
            }
        }
        return fail<ManifestVerificationManualHandoffMaterializeResult>(
            insertOutcome.error()->code, insertOutcome.error()->message);
    }
    candidateCleanup.keep = true;
    return common::Result<ManifestVerificationManualHandoffMaterializeResult>::ok(
        {std::move(handoff), candidateOverlay.value().root});
}

// QML-P4R.2/P4R.5: resolves which Run actually owns the frozen Manifest/
// tasks a handoff for this anchor must verify against - the anchor itself
// for the simple (never paid-repaired) case, or its real origin via the
// durable continuation input when the anchor is itself a paid continuation
// child. Any other anchor workflow fails closed - never guessed. Extracted
// verbatim (behavior unchanged) from materialize()'s own former inline
// preamble so evaluateCreateEligibility() can reuse the identical rule
// without a second, potentially-drifting copy.
common::Result<ManifestVerificationManualHandoffOriginResolution>
ManifestVerificationManualHandoffService::resolveOriginManifestRun(const Run& anchor) const
{
    ManifestVerificationManualHandoffOriginResolution resolution;
    if (anchor.workflowId == kExecutionManifestWorkflowId) {
        resolution.originManifestRunId = anchor.id;
        return common::Result<ManifestVerificationManualHandoffOriginResolution>::ok(
            std::move(resolution));
    }
    if (anchor.workflowId != kManifestVerificationContinuationWorkflowId) {
        return fail<ManifestVerificationManualHandoffOriginResolution>(
            "manual_handoff.unsupported_anchor_workflow",
            "Materializing a manual handoff requires an anchor Run that either owns its own "
            "Manifest or is a paid continuation child of one");
    }

    const auto found = continuations_.findInputByChildRun(anchor.id);
    if (!found.success()) return fail<ManifestVerificationManualHandoffOriginResolution>(
        found.error()->code, found.error()->message);
    if (!found.value().has_value()) {
        return fail<ManifestVerificationManualHandoffOriginResolution>(
            "manual_handoff.origin_unresolvable",
            "No durable continuation input names this anchor as its child; the origin "
            "Manifest Run cannot be resolved");
    }
    resolution.continuationInput = found.value();
    const auto& continuationInput = *resolution.continuationInput;
    // QML-P4R.2 hardening: this service fuses evidence from three
    // independently-stored aggregates - the durable continuation input row,
    // the anchor Run row, and the origin Run row. findInputByChildRun only
    // guarantees the input's OWN internal validity and that childRunId is
    // unique across every input; it proves nothing about whether that row
    // still agrees with the real Run rows. A corrupted/hand-edited row must
    // fail closed here, never be trusted to imply the others agree.
    if (continuationInput.childRunId != anchor.id
        || anchor.parentRunId != std::optional<RunId>(continuationInput.sourceRunId)
        || continuationInput.sourceProjectId != anchor.projectId) {
        return fail<ManifestVerificationManualHandoffOriginResolution>(
            "manual_handoff.origin_identity_conflict",
            "The durable continuation input's identity does not match the anchor Run");
    }
    resolution.originManifestRunId = continuationInput.sourceRunId;
    const auto originRun = runs_.findRun(resolution.originManifestRunId);
    if (!originRun.success()) return fail<ManifestVerificationManualHandoffOriginResolution>(
        originRun.error()->code, originRun.error()->message);
    if (!originRun.value().has_value()
        || originRun.value()->workflowId != kExecutionManifestWorkflowId
        || originRun.value()->projectId != anchor.projectId) {
        return fail<ManifestVerificationManualHandoffOriginResolution>(
            "manual_handoff.origin_identity_conflict",
            "The origin Run named by the continuation input does not exist or does not own "
            "the frozen Manifest for the anchor's own project");
    }
    return common::Result<ManifestVerificationManualHandoffOriginResolution>::ok(
        std::move(resolution));
}

// QML-P4R.5: see this method's own header doc comment for the exact
// contract. eligible defaults to false; every early-return sets a specific
// reasonCode/reasonMessage, never leaving either empty when eligible is
// false.
common::Result<ManifestVerificationManualHandoffCreateEligibility>
ManifestVerificationManualHandoffService::evaluateCreateEligibility(const Run& anchor) const
{
    if (anchor.workflowId != kExecutionManifestWorkflowId
        && anchor.workflowId != kManifestVerificationContinuationWorkflowId) {
        return common::Result<ManifestVerificationManualHandoffCreateEligibility>::ok(
            {false, "manual_handoff.unsupported_anchor_workflow",
             "Materializing a manual handoff requires an anchor Run that either owns its own "
             "Manifest or is a paid continuation child of one"});
    }

    // A resolution failure here is itself one of resolveOriginManifestRun's
    // own stable business-denial codes (unsupported workflow, unresolvable,
    // or identity conflict) for the overwhelming majority of cases - never
    // treated as a hard read error, since the whole point of this call is to
    // answer "can I create," not to require the caller to also handle a
    // distinct failure channel for what is, in practice, always a denial
    // reason materialize() itself would have surfaced identically.
    const auto originResolution = resolveOriginManifestRun(anchor);
    if (!originResolution.success()) {
        return common::Result<ManifestVerificationManualHandoffCreateEligibility>::ok(
            {false, originResolution.error()->code, originResolution.error()->message});
    }
    const RunId originManifestRunId = originResolution.value().originManifestRunId;

    if (anchor.id == originManifestRunId) {
        // The simple case: the SAME read-only admission check materialize()
        // itself calls via admitOrEvaluate() - evaluate() is its already-
        // established non-mutating counterpart (see
        // ManifestVerificationContinuationService's own doc comment),
        // deliberately never admitOrEvaluate() itself, which would create a
        // durable checkpoint as a side effect of a mere read.
        const auto eligibility = admission_.evaluate(anchor.id);
        if (!eligibility.success()) return fail<ManifestVerificationManualHandoffCreateEligibility>(
            eligibility.error()->code, eligibility.error()->message);
        const bool admitted =
            eligibility.value().status == ManifestVerificationContinuationStatus::Admitted;
        return common::Result<ManifestVerificationManualHandoffCreateEligibility>::ok(
            {admitted, eligibility.value().denialCode, eligibility.value().denialMessage});
    }

    // The mixed case: the anchor is itself a paid continuation child, which
    // admission_ always denies (not_manifest_workflow) - materialize()
    // itself evaluates this case directly against the anchor's own Work
    // Ledger entry through the same shared content-repairable-failure check
    // instead, and this eligibility check mirrors exactly that, never a
    // durable checkpoint.
    if (anchor.status != RunStatus::Failed) {
        return common::Result<ManifestVerificationManualHandoffCreateEligibility>::ok(
            {false, "verification_continuation.source_run_not_failed",
             "Manual handoff requires a terminal Failed anchor Run"});
    }
    const auto ledger = workLedger_.findEntry(anchor.id);
    if (!ledger.success()) return fail<ManifestVerificationManualHandoffCreateEligibility>(
        ledger.error()->code, ledger.error()->message);
    if (!ledger.value().has_value()) {
        return common::Result<ManifestVerificationManualHandoffCreateEligibility>::ok(
            {false, "verification_continuation.missing_work_ledger_entry",
             "The anchor Run has no durable Work Ledger entry"});
    }
    if (ledger.value()->runId != anchor.id || ledger.value()->projectId != anchor.projectId
            || ledger.value()->terminalStatus != RunStatus::Failed) {
        return common::Result<ManifestVerificationManualHandoffCreateEligibility>::ok(
            {false, "manual_handoff.origin_identity_conflict",
             "The anchor Run's own Work Ledger entry does not match its identity or is not a "
             "terminal failure"});
    }
    const auto denial = determineContentRepairableFailureDenial(*ledger.value());
    return common::Result<ManifestVerificationManualHandoffCreateEligibility>::ok(
        {denial.eligible(), denial.code, denial.message});
}

// QML-P4R.3.4 hardening: a PRE-EXISTING typed result is never silently
// trusted just because a row is present - its own attemptId/attemptRunId/
// kind must actually agree with what THIS call's own authoritative signal
// (Work Ledger presence/terminalStatus, or the absence of one) expects.
// A mismatch means two disagreeing outcomes exist for the same attempt -
// itself a durable-storage integrity violation this call must never
// silently pick a winner for.
common::Result<void> ManifestVerificationManualHandoffService::checkAttemptResultAgrees(
    const ManifestVerificationManualHandoffAttemptResult& existing,
    const std::string& attemptId, const RunId& attemptRunId,
    ManifestVerificationManualHandoffAttemptResultKind expectedKind)
{
    if (existing.attemptId != attemptId || existing.attemptRunId != attemptRunId
            || existing.kind != expectedKind) {
        return common::Result<void>::failure({
            "manual_handoff.attempt_result_conflict",
            "This attempt's existing typed result does not agree with its own current "
            "authoritative signal - refusing to silently trust either one"});
    }
    return common::Result<void>::ok();
}

// Called BEFORE any Run/stage projection is repaired from the ledger (see
// both call sites' own comments) - a conflict must be caught before any
// mutation, never discovered only after the projection was already
// rewritten to match the (possibly wrong) ledger-implied status.
common::Result<void> ManifestVerificationManualHandoffService::ensureVerifiedAttemptResult(
    const std::string& attemptId, const RunId& attemptRunId, RunStatus ledgerTerminalStatus)
{
    const auto expectedKind = ledgerTerminalStatus == RunStatus::Completed
        ? ManifestVerificationManualHandoffAttemptResultKind::VerifiedSuccess
        : ManifestVerificationManualHandoffAttemptResultKind::VerifiedContentFailure;
    const auto existing = manualHandoffs_.findAttemptResultByAttemptId(attemptId);
    if (!existing.success()) return common::Result<void>::failure(*existing.error());
    if (existing.value().has_value()) {
        return checkAttemptResultAgrees(*existing.value(), attemptId, attemptRunId, expectedKind);
    }
    ManifestVerificationManualHandoffAttemptResult typedResult;
    typedResult.id = "manual-handoff-attempt-result:" + attemptId;
    typedResult.attemptId = attemptId;
    typedResult.attemptRunId = attemptRunId;
    typedResult.kind = expectedKind;
    typedResult.recordedAt = std::chrono::system_clock::now();
    return manualHandoffs_.insertAttemptResult(std::move(typedResult));
}

// ScopeExit-only (see its own call site): a brand-new attempt id, reserved
// moments earlier in the SAME call, can never already carry a typed result
// from any other process - the conflict check here is pure defence in
// depth, not a realistically reachable path.
common::Result<void> ManifestVerificationManualHandoffService::ensureIncompleteAttemptResult(
    const std::string& attemptId, const RunId& attemptRunId)
{
    const auto existing = manualHandoffs_.findAttemptResultByAttemptId(attemptId);
    if (!existing.success()) return common::Result<void>::failure(*existing.error());
    if (existing.value().has_value()) {
        return checkAttemptResultAgrees(*existing.value(), attemptId, attemptRunId,
            ManifestVerificationManualHandoffAttemptResultKind::Incomplete);
    }
    ManifestVerificationManualHandoffAttemptResult typedResult;
    typedResult.id = "manual-handoff-attempt-result:" + attemptId;
    typedResult.attemptId = attemptId;
    typedResult.attemptRunId = attemptRunId;
    typedResult.kind = ManifestVerificationManualHandoffAttemptResultKind::Incomplete;
    typedResult.recordedAt = std::chrono::system_clock::now();
    return manualHandoffs_.insertAttemptResult(std::move(typedResult));
}

// ScopeExit-only, the RejectedBeforeVerification counterpart of
// ensureIncompleteAttemptResult above - same "brand-new attempt id, no
// realistic pre-existing conflict" reasoning.
common::Result<void> ManifestVerificationManualHandoffService::ensureRejectedBeforeVerificationAttemptResult(
    const std::string& attemptId, const RunId& attemptRunId,
    std::string rejectionCode, std::string rejectionMessage)
{
    const auto existing = manualHandoffs_.findAttemptResultByAttemptId(attemptId);
    if (!existing.success()) return common::Result<void>::failure(*existing.error());
    if (existing.value().has_value()) {
        return checkAttemptResultAgrees(*existing.value(), attemptId, attemptRunId,
            ManifestVerificationManualHandoffAttemptResultKind::RejectedBeforeVerification);
    }
    ManifestVerificationManualHandoffAttemptResult typedResult;
    typedResult.id = "manual-handoff-attempt-result:" + attemptId;
    typedResult.attemptId = attemptId;
    typedResult.attemptRunId = attemptRunId;
    typedResult.kind = ManifestVerificationManualHandoffAttemptResultKind::RejectedBeforeVerification;
    typedResult.rejectionCode = std::move(rejectionCode);
    typedResult.rejectionMessage = std::move(rejectionMessage);
    typedResult.recordedAt = std::chrono::system_clock::now();
    return manualHandoffs_.insertAttemptResult(std::move(typedResult));
}

common::Result<ManifestVerificationManualHandoffImportOutcome>
ManifestVerificationManualHandoffService::importAndReverify(
    const std::string& handoffId, std::stop_token stopToken)
{
    OutcomeBriefNudge nudge(outcomeBriefTrigger_);
    std::lock_guard importLock(importMutex_);
    const auto handoffResult = manualHandoffs_.findHandoffById(handoffId);
    if (!handoffResult.success()) return fail<ManifestVerificationManualHandoffImportOutcome>(
        handoffResult.error()->code, handoffResult.error()->message);
    if (!handoffResult.value().has_value()) return fail<ManifestVerificationManualHandoffImportOutcome>(
        "manual_handoff.not_found", "No manual handoff exists with this id");
    const ManifestVerificationManualHandoff& handoff = *handoffResult.value();
    nudge.addAnchor(handoff.verificationAnchorRunId);

    const auto candidateRoot = resolveCandidateRoot(handoff);
    if (!candidateRoot.success()) return fail<ManifestVerificationManualHandoffImportOutcome>(
        candidateRoot.error()->code, candidateRoot.error()->message);

    // Defence against a concurrent writer (e.g. the user's editor still
    // auto-saving) - the same double-capture discipline materialize() uses.
    // A capture failure here also fails closed on a symlink/reparse point
    // introduced into the candidate directory since materialize() - capture()
    // itself already refuses those; this call never re-derives that check.
    const auto candidatePolicy = WorkspaceReadPolicy::secureDefaults(candidateRoot.value());
    const auto firstLive = WorkspaceBaselineProvenance::capture(candidatePolicy);
    const auto secondLive = WorkspaceBaselineProvenance::capture(candidatePolicy);
    if (!firstLive.success() || !secondLive.success()) {
        return fail<ManifestVerificationManualHandoffImportOutcome>(
            "manual_handoff.workspace_unstable",
            "The candidate directory could not be captured");
    }
    if (firstLive.value().fingerprint != secondLive.value().fingerprint) {
        return fail<ManifestVerificationManualHandoffImportOutcome>(
            "manual_handoff.workspace_unstable",
            "The candidate directory changed while being captured");
    }
    const WorkspaceBaselineSnapshot& liveSnapshot = firstLive.value();

    // QML-P4R.3.4: typed retry/replay semantics, decided ONLY from durable
    // state - the Work Ledger's own authoritative entry and this
    // attempt's own ManifestVerificationManualHandoffAttemptResult row -
    // never from an error message's text. Exactly one of three outcomes
    // applies to a prior attempt at this same edited fingerprint:
    // VerifiedSuccess/VerifiedContentFailure (the ledger's own entry;
    // replay reconciles the Run/stage/typed-result projections from it,
    // never re-verifies) or Incomplete (a transient/tool/infrastructure
    // failure or a cancellation; a new ordinal retry of the SAME edited
    // bytes is always permitted). A still-active reservation blocks a
    // concurrent retry. An attempt whose Run reached a terminal status
    // with NEITHER a ledger entry NOR a typed result fails closed - see
    // ManifestVerificationManualHandoffAttemptResultKind's own doc
    // comment for why this must never be guessed.
    const auto existingAttempt = manualHandoffs_.findAttemptByFingerprint(
        handoff.id, liveSnapshot.fingerprint);
    if (!existingAttempt.success()) return fail<ManifestVerificationManualHandoffImportOutcome>(
        existingAttempt.error()->code, existingAttempt.error()->message);
    if (existingAttempt.value().has_value()) {
        const auto& priorAttempt = *existingAttempt.value();
        const auto entry = workLedger_.findEntry(priorAttempt.attemptRunId);
        if (!entry.success()) return fail<ManifestVerificationManualHandoffImportOutcome>(
            entry.error()->code, entry.error()->message);
        if (entry.value().has_value()) {
            // Ledger-first finalization is intentional. If the process died
            // after committing the authoritative verification but before the
            // Run/stage/typed-result projections that ordinarily follow it,
            // repair each of them now, independently, rather than creating
            // another attempt for already-verified work.
            const auto existingRun = runs_.findRun(priorAttempt.attemptRunId);
            if (!existingRun.success()) return fail<ManifestVerificationManualHandoffImportOutcome>(
                existingRun.error()->code, existingRun.error()->message);
            if (!existingRun.value().has_value()) {
                return fail<ManifestVerificationManualHandoffImportOutcome>(
                    "manual_handoff.attempt_corrupt",
                    "The completed manual-handoff attempt has no Run");
            }
            // QML-P4R.3.4 hardening: validated BEFORE the Run/stage
            // projection is touched, never after - a conflict (an
            // existing typed result that disagrees with what this
            // ledger entry's own terminalStatus implies) must be caught
            // before any mutation, not discovered only once the
            // projection has already been rewritten to match it.
            const auto typedResultAgrees = ensureVerifiedAttemptResult(
                priorAttempt.id, priorAttempt.attemptRunId, entry.value()->terminalStatus);
            if (!typedResultAgrees.success()) return fail<ManifestVerificationManualHandoffImportOutcome>(
                typedResultAgrees.error()->code, typedResultAgrees.error()->message);
            Run repairedRun = *existingRun.value();
            if (repairedRun.status != entry.value()->terminalStatus) {
                repairedRun.status = entry.value()->terminalStatus;
                repairedRun.progress = repairedRun.status == RunStatus::Completed ? 1.0 : 0.0;
                repairedRun.currentStageId.reset();
                repairedRun.finishedAt = std::chrono::system_clock::now();
                repairedRun.errorMessage = repairedRun.status == RunStatus::Completed
                    ? std::string{} : "Manual handoff verification did not pass";
                const auto updated = runs_.updateRun(std::move(repairedRun));
                if (!updated.success()) return fail<ManifestVerificationManualHandoffImportOutcome>(
                    updated.error()->code, updated.error()->message);
            }
            // Reconcile the stage independently from the Run. A previous
            // process may have committed either projection first; nesting
            // this under the Run-status mismatch would leave a stale Running
            // stage forever when only that second update was interrupted.
            const auto stages = runs_.listStages(priorAttempt.attemptRunId);
            if (!stages.success()) return fail<ManifestVerificationManualHandoffImportOutcome>(
                stages.error()->code, stages.error()->message);
            for (auto stage : stages.value()) {
                const StageStatus expectedStageStatus =
                    entry.value()->terminalStatus == RunStatus::Completed
                    ? StageStatus::Completed : StageStatus::Failed;
                if (stage.status != expectedStageStatus) {
                    stage.status = expectedStageStatus;
                    stage.progress = stage.status == StageStatus::Completed ? 1.0 : 0.0;
                    stage.finishedAt = std::chrono::system_clock::now();
                    stage.errorMessage = stage.status == StageStatus::Completed
                        ? std::string{} : "Manual handoff verification did not pass";
                    const auto updatedStage = runs_.updateStage(std::move(stage));
                    if (!updatedStage.success()) return fail<ManifestVerificationManualHandoffImportOutcome>(
                        updatedStage.error()->code, updatedStage.error()->message);
                }
            }
            proposals_.releaseRunClaims(priorAttempt.attemptRunId);
            return common::Result<ManifestVerificationManualHandoffImportOutcome>::ok({
                priorAttempt.attemptRunId, entry.value()->terminalStatus,
                entry.value()->verification, /*idempotentReplay=*/true});
        }
        const auto existingRun = runs_.findRun(priorAttempt.attemptRunId);
        if (!existingRun.success()) return fail<ManifestVerificationManualHandoffImportOutcome>(
            existingRun.error()->code, existingRun.error()->message);
        if (existingRun.value().has_value()
                && isActiveAttemptStatus(existingRun.value()->status)) {
            return fail<ManifestVerificationManualHandoffImportOutcome>(
                "manual_handoff.attempt_in_progress",
                "This exact edited candidate is already being re-verified");
        }
        if (existingRun.value().has_value()) {
            // The Run reached a terminal status with no ledger entry -
            // consult this attempt's own typed result to know whether a
            // retry is safe, rather than assuming "no ledger" always means
            // retryable (see the corrupt-result fail-closed guard below).
            const auto attemptResult = manualHandoffs_.findAttemptResultByAttemptId(priorAttempt.id);
            if (!attemptResult.success()) return fail<ManifestVerificationManualHandoffImportOutcome>(
                attemptResult.error()->code, attemptResult.error()->message);
            if (!attemptResult.value().has_value()) {
                return fail<ManifestVerificationManualHandoffImportOutcome>(
                    "manual_handoff.attempt_result_missing",
                    "This manual-handoff attempt reached a terminal state with neither an "
                    "authoritative Work Ledger entry nor a durable typed result - refusing to "
                    "guess whether it may be retried");
            }
            if (attemptResult.value()->attemptRunId != priorAttempt.attemptRunId) {
                return fail<ManifestVerificationManualHandoffImportOutcome>(
                    "manual_handoff.attempt_result_conflict",
                    "This manual-handoff attempt's typed result is recorded against a different "
                    "attempt Run");
            }
            // QML-P4R.3.4 hardening: RejectedBeforeVerification is a
            // deterministic, stable verdict on THIS exact edited
            // fingerprint (see its own doc comment) - a later call must
            // replay it verbatim, never allocate a new ordinal the same
            // way it never replays a genuine Incomplete transient
            // failure into a fresh attempt.
            if (attemptResult.value()->kind
                    == ManifestVerificationManualHandoffAttemptResultKind::RejectedBeforeVerification) {
                return fail<ManifestVerificationManualHandoffImportOutcome>(
                    attemptResult.value()->rejectionCode.value_or("manual_handoff.content_rejected"),
                    attemptResult.value()->rejectionMessage.value_or(
                        "This exact edited candidate was previously rejected before verification"));
            }
            if (attemptResult.value()->kind
                    != ManifestVerificationManualHandoffAttemptResultKind::Incomplete) {
                return fail<ManifestVerificationManualHandoffImportOutcome>(
                    "manual_handoff.attempt_result_corrupt",
                    "This manual-handoff attempt's typed result claims a verified outcome but no "
                    "authoritative Work Ledger entry exists for it");
            }
        }
        // Incomplete typed result, or no Run row at all (crashed between
        // insertAttempt() and insertRunWithStages(), permanently harmless
        // dead audit history): fall through and reserve a new ordinal
        // retry of this exact edited snapshot.
    }

    // Every map below is keyed by detail::normalizedPathKey - the same
    // lowercase-normalized identity attribution/proposal matching already
    // uses throughout this class (see materialize()'s own allowedPaths
    // loop) - never a raw, potentially differently-cased filesystem path.
    // WorkspaceBaselineSnapshot entries carry their real on-disk case, which
    // is not guaranteed to match a proposal's own submitted case.
    std::unordered_map<std::string, WorkspaceBaselineEntry> originalByPath;
    for (const auto& file : handoff.originalSnapshot.files) {
        originalByPath.emplace(
            detail::normalizedPathKey(detail::pathFromUtf8(file.relativePath)), file);
    }
    std::unordered_map<std::string, WorkspaceBaselineEntry> liveByPath;
    for (const auto& file : liveSnapshot.files) {
        liveByPath.emplace(
            detail::normalizedPathKey(detail::pathFromUtf8(file.relativePath)), file);
    }
    std::unordered_map<std::string, ManifestVerificationManualHandoffAllowedPath> allowedByPath;
    for (const auto& allowed : handoff.allowedPaths) {
        allowedByPath.emplace(
            detail::normalizedPathKey(detail::pathFromUtf8(allowed.relativePath)), allowed);
    }

    for (const auto& [path, entry] : liveByPath) {
        if (!originalByPath.contains(path)) {
            return fail<ManifestVerificationManualHandoffImportOutcome>(
                "manual_handoff.new_path_not_allowed",
                "The candidate directory contains a path that was not part of the frozen handoff: "
                    + entry.relativePath);
        }
    }
    std::vector<std::string> changedPaths;
    for (const auto& [path, original] : originalByPath) {
        const auto live = liveByPath.find(path);
        if (live == liveByPath.end()) {
            return fail<ManifestVerificationManualHandoffImportOutcome>(
                "manual_handoff.path_deleted_not_allowed",
                "A frozen candidate path was deleted - deletion is not supported by manual "
                "handoff: " + original.relativePath);
        }
        if (live->second.sha256 == original.sha256) continue;
        if (!allowedByPath.contains(path)) {
            return fail<ManifestVerificationManualHandoffImportOutcome>(
                "manual_handoff.forbidden_path_changed",
                "A candidate path outside the editable allowlist was changed: "
                    + original.relativePath);
        }
        changedPaths.push_back(path);
    }
    if (changedPaths.empty()) {
        return fail<ManifestVerificationManualHandoffImportOutcome>(
            "manual_handoff.no_changes_to_reverify",
            "No editable path in the candidate directory has actually changed");
    }

    const auto project = projects_.findById(handoff.sourceProjectId);
    if (!project.success()) return fail<ManifestVerificationManualHandoffImportOutcome>(
        project.error()->code, project.error()->message);
    if (!project.value().has_value()) {
        return fail<ManifestVerificationManualHandoffImportOutcome>(
            "manual_handoff.source_project_missing", "The source project does not exist");
    }
    const auto sourceRun = runs_.findRun(handoff.originManifestRunId);
    if (!sourceRun.success()) return fail<ManifestVerificationManualHandoffImportOutcome>(
        sourceRun.error()->code, sourceRun.error()->message);
    if (!sourceRun.value().has_value()) {
        return fail<ManifestVerificationManualHandoffImportOutcome>(
            "manual_handoff.source_missing", "The origin Manifest Run does not exist");
    }
    const auto workspaceRoot =
        sourceRun.value()->workspacePath.value_or(project.value()->rootPath);
    // The validated root of the temporary verification copies: a root that is not usable is refused before the workspace is
    // captured or anything is copied or run.
    const auto ephemeralRoot = resolveVerificationScratchRoot(scratchProvider_, ephemeralScratchRoot_, workspaceRoot);
    if (!ephemeralRoot.success()) return fail<ManifestVerificationManualHandoffImportOutcome>(
        ephemeralRoot.error()->code, ephemeralRoot.error()->message);

    // Captured once, early - the same single-capture-reused-for-every-
    // overlay-build discipline ManifestVerificationContinuationRepairService
    // itself uses, and reused below for the final overlay build rather than
    // recaptured. The trusted proposal store lives beside the app's own
    // database (BackendRuntime::openProposalStorage), never inside the real
    // project workspace, so a successor's own publish never perturbs this.
    const auto finalBaselinePolicy = WorkspaceReadPolicy::secureDefaults(workspaceRoot);
    const auto finalBaseline = WorkspaceBaselineProvenance::capture(finalBaselinePolicy);
    if (!finalBaseline.success()) return fail<ManifestVerificationManualHandoffImportOutcome>(
        finalBaseline.error()->code, finalBaseline.error()->message);
    // The real project workspace must still match exactly what this handoff
    // was frozen against - no implicit rebase, the same discipline
    // materialize() itself enforces. Unlike materialize(), this call never
    // attempts a legacy-drift normalization: a handoff that has gone stale
    // against a since-changed live workspace must be re-materialized, not
    // silently carried forward here.
    if (finalBaseline.value().fingerprint != handoff.baselineFingerprint) {
        return fail<ManifestVerificationManualHandoffImportOutcome>(
            "manual_handoff.baseline_drift",
            "The workspace no longer matches the handoff's frozen baseline; implicit rebase is "
            "forbidden");
    }

    const auto manifest = manifests_.findForRun(handoff.originManifestRunId);
    if (!manifest.success()) return fail<ManifestVerificationManualHandoffImportOutcome>(
        manifest.error()->code, manifest.error()->message);
    if (!manifest.value().has_value()) {
        return fail<ManifestVerificationManualHandoffImportOutcome>(
            "manual_handoff.source_corrupt", "The source Manifest is missing");
    }

    // Reserve the immutable attempt BEFORE publishing any successor, Run,
    // or Work-Ledger state. This is the crash/idempotency boundary: two
    // concurrent imports of the same handoff cannot both allocate the same
    // ordinal, while an incomplete reserved attempt remains durable audit
    // evidence and a later call may allocate a new ordinal retry.
    const auto priorAttempts = manualHandoffs_.listAttemptsForHandoff(handoff.id);
    if (!priorAttempts.success()) return fail<ManifestVerificationManualHandoffImportOutcome>(
        priorAttempts.error()->code, priorAttempts.error()->message);

    const RunId attemptRunId = generateUuid();
    const auto attemptStartedAt = std::chrono::system_clock::now();
    ManifestVerificationManualHandoffAttempt attempt;
    attempt.id = "manual-handoff-attempt:" + attemptRunId;
    attempt.handoffId = handoff.id;
    attempt.verificationAnchorRunId = handoff.verificationAnchorRunId;
    attempt.attemptRunId = attemptRunId;
    attempt.attemptOrdinal = priorAttempts.value().size() + 1;
    if (!priorAttempts.value().empty()) attempt.previousAttemptId = priorAttempts.value().back().id;
    attempt.editedSnapshotFingerprint = liveSnapshot.fingerprint;
    attempt.createdAt = attemptStartedAt;
    const auto insertedAttempt = manualHandoffs_.insertAttempt(attempt);
    if (!insertedAttempt.success()) return fail<ManifestVerificationManualHandoffImportOutcome>(
        insertedAttempt.error()->code, insertedAttempt.error()->message);

    Run attemptRun = *sourceRun.value();
    attemptRun.id = attemptRunId;
    attemptRun.parentRunId = handoff.verificationAnchorRunId;
    attemptRun.title = sourceRun.value()->title + " - manual handoff reverify";
    attemptRun.workflowId = std::string(kManifestVerificationManualHandoffWorkflowId);
    attemptRun.status = RunStatus::Running;
    attemptRun.progress = 0.0;
    attemptRun.currentStageId = attemptRunId + ":manual-handoff-reverify";
    attemptRun.errorMessage.clear();
    attemptRun.usage = {};
    attemptRun.providerBudgetUsage = {};
    attemptRun.createdAt = attemptStartedAt;
    attemptRun.startedAt = attemptStartedAt;
    attemptRun.finishedAt.reset();
    attemptRun.revision = 0;

    RunStage stage;
    stage.id = *attemptRun.currentStageId;
    stage.runId = attemptRunId;
    stage.type = "manifest-verification-manual-handoff-reverify";
    stage.title = "Manual handoff reverify";
    stage.description = "Backend-verified user-edited candidate; no provider/model call";
    stage.status = StageStatus::Running;
    stage.progress = 0.0;
    stage.startedAt = attemptStartedAt;
    const auto insertedRun = runs_.insertRunWithStages(attemptRun, {stage});
    if (!insertedRun.success()) return fail<ManifestVerificationManualHandoffImportOutcome>(
        insertedRun.error()->code, insertedRun.error()->message);

    std::vector<ChangeSetId> createdSuccessorIds;
    // QML-P4R.3.4 hardening: set ONLY by the specific, structurally-known
    // call sites below whose failure is a deterministic judgment of the
    // edited bytes themselves (never by inspecting an error's text) -
    // see ManifestVerificationManualHandoffAttemptResultKind::
    // RejectedBeforeVerification's own doc comment. Every OTHER failure
    // in this reserved scope (a repository read, overlay build, or the
    // final verify() call's own structural failure) leaves this unset,
    // so ScopeExit below still classifies it Incomplete/retryable.
    std::optional<common::Error> pendingContentRejection;
    ScopeExit incompleteAttempt([&] {
        const bool cancelled = stopToken.stop_requested();
        for (const auto& id : createdSuccessorIds) {
            static_cast<void>(proposals_.markMissingEvidence(
                id, "manual_handoff.attempt_incomplete",
                "The manual-handoff attempt ended before authoritative verification was stored"));
        }
        proposals_.releaseRunClaims(attemptRunId);
        const auto storedStages = runs_.listStages(attemptRunId);
        if (storedStages.success()) {
            for (auto failedStage : storedStages.value()) {
                failedStage.status = cancelled ? StageStatus::Cancelled : StageStatus::Failed;
                failedStage.progress = 0.0;
                failedStage.finishedAt = std::chrono::system_clock::now();
                failedStage.errorMessage = cancelled
                    ? "Manual handoff reverify was cancelled"
                    : "Manual handoff reverify did not reach an authoritative result";
                static_cast<void>(runs_.updateStage(std::move(failedStage)));
            }
        }
        const auto storedRun = runs_.findRun(attemptRunId);
        if (storedRun.success() && storedRun.value().has_value()) {
            auto failedRun = *storedRun.value();
            failedRun.status = cancelled ? RunStatus::Cancelled : RunStatus::Failed;
            failedRun.progress = 0.0;
            failedRun.currentStageId.reset();
            failedRun.finishedAt = std::chrono::system_clock::now();
            failedRun.errorMessage = cancelled
                ? "Manual handoff reverify was cancelled"
                : "Manual handoff reverify did not reach an authoritative result";
            static_cast<void>(runs_.updateRun(std::move(failedRun)));
        }
        // QML-P4R.3.4: this attempt ended WITHOUT ever producing a Work
        // Ledger entry. Two distinct typed outcomes are possible, never
        // decided from error text - only from which specific call
        // (recorded structurally in pendingContentRejection above,
        // never re-derived here from any error's message) actually
        // failed:
        //   - pendingContentRejection set: a deterministic, pre-
        //     verification content/policy rejection of the edited bytes
        //     themselves - RejectedBeforeVerification, with its own
        //     stable (code, message) a later call replays verbatim.
        //   - unset: a genuine transient/tool/infrastructure failure or
        //     cancellation - Incomplete, letting a later call retry the
        //     same edited bytes under a new ordinal.
        // Best-effort, like this entire ScopeExit: no Result channel
        // exists at destructor time. A failure here is not silently
        // unsafe - importAndReverify's own lookup fails closed
        // (manual_handoff.attempt_result_missing) rather than guessing
        // if this row never lands, and reconcileInterruptedAttempts
        // writes Incomplete durably (with a real Result channel) for a
        // crash that outlived this process - it can only ever conclude
        // Incomplete, since pendingContentRejection is in-memory only and
        // never survives a crash; see its own doc comment.
        if (pendingContentRejection.has_value()) {
            static_cast<void>(ensureRejectedBeforeVerificationAttemptResult(
                attempt.id, attemptRunId, pendingContentRejection->code,
                pendingContentRejection->message));
        } else {
            static_cast<void>(ensureIncompleteAttemptResult(attempt.id, attemptRunId));
        }
    });

    // Group by owning proposal - one materializeRepairSuccessor call per
    // amended proposal, never per file, so two edited files belonging to
    // the same proposal are amended together in one successor.
    std::unordered_map<ChangeSetId, std::vector<std::string>> pathsByProposal;
    for (const auto& path : changedPaths) pathsByProposal[allowedByPath.at(path).ownerProposalId].push_back(path);

    // Each frozen member carries ITS OWN real owning Run - never a single
    // handoff-wide value - so a successor's expected predecessor Run is
    // always the Run that actually produced the proposal it amends, even
    // once QML-P4R.2 lets a handoff's frozen set mix an origin Manifest Run
    // with a paid continuation child.
    std::unordered_map<ChangeSetId, RunId> owningRunByProposalId;
    for (const auto& member : handoff.frozenMembers) {
        owningRunByProposalId.emplace(member.proposalId, member.owningRunId);
    }

    std::unordered_map<ChangeSetId, ChangeSetId> successorByOriginal;
    for (const auto& [ownerProposalId, paths] : pathsByProposal) {
        const auto currentChanges = proposals_.listChanges(ownerProposalId);
        if (!currentChanges.success()) return fail<ManifestVerificationManualHandoffImportOutcome>(
            currentChanges.error()->code, currentChanges.error()->message);

        // QML-P4R.6 follow-up hardening: the per-edited-path check further
        // below (inside the `paths` loop) only ever examines a Create/Modify
        // entry this round actually EDITS - a carried-forward Modify/Delete/
        // Rename entry that is NOT part of this round's own edits never
        // reaches it (a Delete in particular can never be "edited" content,
        // so it is never matched there at all). Scan the frozen predecessor's
        // ENTIRE change list up front instead, so a corrupt Modify/Delete/
        // Rename entry anywhere in it - edited or merely carried forward -
        // fails closed with this call's own stable, diagnosable code before
        // materializeRepairSuccessor() is ever invoked, rather than silently
        // flowing into that call's validator (surfacing later as an opaque
        // rejection - see the explicit ReadyForReview state check below) or
        // an even more opaque downstream overlay failure. Format-validated
        // exactly like ChangeProposalValidator::checkOperationInvariants
        // itself (a 64-character hex digest) - not merely presence-checked -
        // so a present but corrupt/truncated hash is caught here too.
        for (const auto& change : currentChanges.value()) {
            if (change.operation != FileChangeOperation::Modify
                && change.operation != FileChangeOperation::Delete
                && change.operation != FileChangeOperation::Rename) {
                continue;
            }
            const auto isHexDigit = [](char c) {
                return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
            };
            const bool malformed = !change.expectedOriginalHash.has_value()
                || change.expectedOriginalHash->size() != 64
                || !std::all_of(
                       change.expectedOriginalHash->begin(), change.expectedOriginalHash->end(),
                       isHexDigit);
            if (malformed) {
                pendingContentRejection = common::Error {
                    "manual_handoff.predecessor_hash_evidence_missing",
                    "The frozen predecessor's own Modify/Delete/Rename change carries no valid "
                    "expectedOriginalHash - its durable record is corrupt"};
                return fail<ManifestVerificationManualHandoffImportOutcome>(
                    pendingContentRejection->code, pendingContentRejection->message);
            }
        }

        nlohmann::json changesJson = nlohmann::json::array();
        std::unordered_map<std::string, std::string> observedFileHashes;
        for (const auto& path : paths) {
            const auto matchedChange = std::find_if(
                currentChanges.value().begin(), currentChanges.value().end(),
                [&](const ProposedFileChange& change) {
                    return detail::normalizedPathKey(change.relativePath) == path
                        && (change.operation == FileChangeOperation::Create
                            || change.operation == FileChangeOperation::Modify);
                });
            if (matchedChange == currentChanges.value().end()) {
                // QML-P4R.3.4 hardening: a deterministic mismatch between
                // this handoff's own frozen allowedPaths contract and the
                // owning proposal's CURRENT (append-only, never mutated
                // in place) recorded changes - for the SAME edited
                // fingerprint this can never resolve differently on a
                // bare retry, so it is a stable, replay-eligible
                // rejection, never a transient one.
                pendingContentRejection = common::Error {
                    "manual_handoff.source_proposal_unavailable",
                    "An editable path's owning proposal no longer carries a matching "
                    "Create/Modify target"};
                return fail<ManifestVerificationManualHandoffImportOutcome>(
                    pendingContentRejection->code, pendingContentRejection->message);
            }
            // Hash-evidence validity for this matched Modify entry was
            // already proven by the full-predecessor pre-check above (which
            // covers every Modify/Delete/Rename in currentChanges, not just
            // ones this round edits) - nothing further to check here. Left
            // deliberately unreachable-in-practice via the ordinary public
            // creation API alone, exactly as that check's own doc comment
            // explains - defense in depth against a corrupted durable row
            // from any other source.
            // `path` is a lowercase comparison key only. Use the baseline's
            // real on-disk spelling for filesystem access so Linux/macOS do
            // not turn a valid edit into a false unreadable-file failure.
            const auto edited = readEditedFile(candidateRoot.value()
                / detail::pathFromUtf8(liveByPath.at(path).relativePath));
            if (!edited.success()) {
                // QML-P4R.3.4 hardening: edited_file_too_large is a pure,
                // deterministic function of the edited bytes' own size at
                // this exact fingerprint - always the same verdict on
                // retry, so it is a stable rejection. edited_file_unreadable
                // (open/read failing moments after this same candidate was
                // captured twice successfully) reads as a genuine transient
                // I/O condition instead, so it stays Incomplete/retryable.
                // Distinguished here by this OWN function's own closed,
                // fully-controlled set of stable error codes it just
                // returned two lines above readEditedFile's own
                // definition - never by inspecting free-form error text.
                if (edited.error()->code == "manual_handoff.edited_file_too_large") {
                    pendingContentRejection = *edited.error();
                }
                return fail<ManifestVerificationManualHandoffImportOutcome>(
                    edited.error()->code, edited.error()->message);
            }
            changesJson.push_back({
                {"change_id", "manual-" + generateShortToken()},
                {"operation", toString(matchedChange->operation)},
                // The current proposal's own real, as-submitted case/
                // separators - not the normalized lookup key above, which
                // is comparison-only.
                {"relative_path", matchedChange->relativePath.generic_string()},
                {"source_path", nullptr},
                {"proposed_content", edited.value()}});
            // QML-P4R.6 hardening: the trusted hash evidence a Modify/Delete
            // precondition is checked against is the REAL WORKSPACE file's
            // own content - never the materialized candidate's. For a
            // Modify predecessor the candidate already contains the
            // predecessor's own PROPOSED new content (baseline + proposals
            // applied is exactly what materialize() built it from), which
            // is not what ChangeProposalValidator::checkHash actually reads
            // (projectRoot / relativePath, the live workspace) - using the
            // candidate's own originalByPath hash here previously produced
            // a false proposal.hash_mismatch for every edited Modify path
            // amending an already-Modify predecessor (never surfaced by any
            // prior test, since every one used a Create-only predecessor,
            // which carries no such precondition at all). The correct,
            // already-durable value is the predecessor's own change's own
            // expectedOriginalHash - carried forward verbatim, exactly the
            // same evidence the ORIGINAL proposal was itself bound and
            // validated against; a Create has none (matchedChange's own
            // expectedOriginalHash stays unset) and needs no entry here.
            if (matchedChange->expectedOriginalHash.has_value()) {
                observedFileHashes.emplace(
                    detail::normalizedPathKey(detail::pathFromUtf8(path)),
                    *matchedChange->expectedOriginalHash);
            }
        }
        const nlohmann::json payload {
            {"schemaVersion", std::string(kProposalSubmissionSchemaVersion)},
            {"summary", "Manual handoff edit"},
            {"changes", std::move(changesJson)}};

        MaterializeRepairSuccessorRequest request;
        request.runId = attemptRunId;
        request.projectId = handoff.sourceProjectId;
        // Honest, non-fictitious provenance (never a fabricated provider/
        // model execution) - see this class's own doc comment.
        request.providerId = "manual-handoff";
        request.modelId = "manual-handoff";
        request.agentRole = "user";
        request.predecessorChangeSetId = ownerProposalId;
        request.repairRawModelOutput = payload.dump();
        request.policy.projectRoot = workspaceRoot;
        // QML-P4R.6 hardening: authorizes the trusted union of (a) the
        // paths actually being edited this round and (b) every path the
        // FROZEN PREDECESSOR proposal already owns - never anything wider.
        // A predecessor may legitimately touch more paths than
        // handoff.allowedPaths exposes as editable (only paths still named
        // by a remaining owner finding are editable - see materialize()'s
        // own doc comment); materializeRepairSuccessor() always carries
        // those untouched predecessor paths forward verbatim, so excluding
        // them here would make every multi-path predecessor's own
        // unedited paths fail ChangeProposalValidator's allowedRoots check
        // with a false proposal.outside_allowed_roots. The verification
        // loop below independently proves every carried-forward path in
        // the resulting successor is byte-identical to the predecessor's
        // own recorded content before this successor is ever trusted -
        // widening allowedRoots here only lets the validator ACCEPT the
        // shape; it never substitutes for that verification.
        for (const auto& change : currentChanges.value()) {
            request.policy.allowedRoots.push_back(change.relativePath);
            if (change.sourcePath.has_value()) {
                request.policy.allowedRoots.push_back(*change.sourcePath);
            }
        }
        request.policy.allowedRootsAreExactPaths = true;
        request.expectedPredecessorRunId = owningRunByProposalId.at(ownerProposalId);
        request.observedFileHashes = std::move(observedFileHashes);

        const auto successor = proposals_.materializeRepairSuccessor(std::move(request));
        if (!successor.success()) {
            // QML-P4R.3.4 hardening: materializeRepairSuccessor's own
            // validate-and-persist pipeline (schema, safety scan,
            // allowedRoots/hash-evidence policy) is exactly where the
            // user's actual edit gets judged - a rejection here is a
            // deterministic verdict on the edited bytes for this frozen
            // predecessor/policy, never a transient condition; the same
            // bytes will fail identically on a bare retry. Classified by
            // WHICH CALL failed, never by inspecting its returned error's
            // text.
            pendingContentRejection = *successor.error();
            return fail<ManifestVerificationManualHandoffImportOutcome>(
                successor.error()->code, successor.error()->message);
        }
        // QML-P4R.6 follow-up hardening: validateAndPersist() persists a
        // Rejected (or, for an empty successor - structurally unreachable
        // here per materializeChangeList's own size invariant, but guarded
        // regardless - NoChangesNeeded) change set as a "successful" Result;
        // only cs.state (never Result::success()) distinguishes a genuine
        // validator rejection (e.g. proposal.cross_agent_conflict between
        // two owning proposals' successors racing for the same carried-
        // forward path under this same attempt) from a real ReadyForReview
        // successor. Checked BEFORE createdSuccessorIds/successorByOriginal
        // are ever touched and before the carried-forward verification loop
        // or the final overlay build below - trusting success() alone would
        // let a Rejected/NoChangesNeeded successor flow into all of that as
        // if it were real, surfacing later as an opaque downstream failure
        // instead of this call's own diagnosable rejection. Setting
        // pendingContentRejection here (exactly like every other
        // deterministic-verdict-on-the-edited-bytes rejection in this
        // function) is what makes the ScopeExit cleanup classify this
        // attempt RejectedBeforeVerification, never Incomplete - a stable,
        // replay-eligible outcome, not a retryable one. The successor's own
        // path claim is already released internally by validateAndPersist
        // itself for any non-ReadyForReview terminal state, so no cleanup
        // beyond that stable classification is owed here.
        if (successor.value().state != ChangeSetState::ReadyForReview) {
            std::string findingCodes;
            for (const auto& finding : successor.value().validationFindings) {
                if (!findingCodes.empty()) findingCodes += "; ";
                findingCodes += finding.code;
            }
            pendingContentRejection = common::Error {
                "manual_handoff.successor_rejected",
                findingCodes.empty()
                    ? "The materialized successor was rejected by validation"
                    : "The materialized successor was rejected by validation: " + findingCodes};
            return fail<ManifestVerificationManualHandoffImportOutcome>(
                pendingContentRejection->code, pendingContentRejection->message);
        }
        createdSuccessorIds.push_back(successor.value().changeSetId);

        // QML-P4R.6 hardening: the allowedRoots widening above only lets
        // the validator ACCEPT a wider path set - it never proves a
        // carried-forward path's own content actually IS the predecessor's
        // unchanged content. Independently re-derive and verify every
        // successor change whose path was not part of this round's own
        // edits: it must be the literal predecessor entry (same changeId,
        // operation, relativePath, sourcePath) with byte-identical content,
        // and no path may exist in the successor that is neither an edited
        // path nor a predecessor path. Never trusts
        // materializeChangeList()'s own internal correctness alone - this
        // is this call's own trust boundary, the one thing standing
        // between "the user edited one file" and "the successor silently
        // carries a fourth, unauthorized path." Deliberately NOT provable
        // to be reachable through this class's own public API alone - by
        // construction, materializeChangeList() always copies a carried-
        // forward predecessor change verbatim (same changeId, same already-
        // published content) and this class never re-derives one from
        // anything model/agent-authored - this is defense in depth against
        // a FUTURE regression in that merge logic, not a currently-
        // exploitable gap.
        const auto successorChanges = proposals_.listChanges(successor.value().changeSetId);
        if (!successorChanges.success()) {
            pendingContentRejection = *successorChanges.error();
            return fail<ManifestVerificationManualHandoffImportOutcome>(
                successorChanges.error()->code, successorChanges.error()->message);
        }
        std::unordered_map<std::string, const ProposedFileChange*> predecessorByPath;
        for (const auto& change : currentChanges.value()) {
            predecessorByPath.emplace(detail::normalizedPathKey(change.relativePath), &change);
        }
        std::unordered_set<std::string> seenPredecessorPaths;
        auto rejectCarriedForwardMismatch = [&] {
            pendingContentRejection = common::Error {
                "manual_handoff.carried_forward_change_mismatch",
                "A successor path outside this round's own edits does not match the frozen "
                "predecessor's own recorded change exactly"};
            return fail<ManifestVerificationManualHandoffImportOutcome>(
                pendingContentRejection->code, pendingContentRejection->message);
        };
        for (const auto& successorChange : successorChanges.value()) {
            const auto key = detail::normalizedPathKey(successorChange.relativePath);
            if (std::find(paths.begin(), paths.end(), key) != paths.end()) {
                // One of this round's own edited paths - its new content is
                // expected to differ from the predecessor; already fully
                // judged by materializeRepairSuccessor's own validation
                // pipeline above.
                continue;
            }
            const auto predecessor = predecessorByPath.find(key);
            if (predecessor == predecessorByPath.end()) return rejectCarriedForwardMismatch();
            const auto* predecessorChange = predecessor->second;
            if (successorChange.changeId != predecessorChange->changeId
                || successorChange.operation != predecessorChange->operation
                || successorChange.relativePath != predecessorChange->relativePath
                || successorChange.sourcePath != predecessorChange->sourcePath
                || successorChange.expectedOriginalHash != predecessorChange->expectedOriginalHash) {
                return rejectCarriedForwardMismatch();
            }
            if (successorChange.operation == FileChangeOperation::Create
                || successorChange.operation == FileChangeOperation::Modify) {
                const auto predecessorContent = proposals_.readProposedContent(
                    ownerProposalId, predecessorChange->changeId);
                const auto successorContent = proposals_.readProposedContent(
                    successor.value().changeSetId, successorChange.changeId);
                if (!predecessorContent.success() || !successorContent.success()
                    || predecessorContent.value() != successorContent.value()) {
                    return rejectCarriedForwardMismatch();
                }
            }
            seenPredecessorPaths.insert(key);
        }
        // Every predecessor path not part of this round's own edits must
        // still be present in the successor - materializeChangeList()'s own
        // size invariant already guards the general case, but this proves
        // it for THIS specific owner proposal's own exact path set.
        for (const auto& predecessorEntry : predecessorByPath) {
            const auto& key = predecessorEntry.first;
            const bool wasEdited = std::find(paths.begin(), paths.end(), key) != paths.end();
            if (!wasEdited && !seenPredecessorPaths.contains(key)) {
                return rejectCarriedForwardMismatch();
            }
        }

        successorByOriginal.emplace(ownerProposalId, successor.value().changeSetId);
    }

    std::vector<ChangeSetId> finalProposalIds;
    finalProposalIds.reserve(handoff.frozenMembers.size());
    for (const auto& member : handoff.frozenMembers) {
        const auto successor = successorByOriginal.find(member.proposalId);
        finalProposalIds.push_back(
            successor == successorByOriginal.end() ? member.proposalId : successor->second);
    }

    // Verification always runs against a FRESH, clean overlay built from the
    // trusted proposal store - never the editable candidate directory
    // itself (see this call's own doc comment). Reuses the SAME baseline
    // captured above, before any successor existed - never a second,
    // later capture (see that capture's own doc comment for why).
    BuildManifestProposalOverlayRequest finalRequest;
    finalRequest.workspaceRoot = workspaceRoot;
    finalRequest.trustedScratchRoot = ephemeralRoot.value();
    finalRequest.workspaceIdentity = handoff.workspaceIdentity;
    finalRequest.baselinePolicy = finalBaselinePolicy;
    finalRequest.baselineSnapshot = finalBaseline.value();
    finalRequest.orderedProposalIds = finalProposalIds;
    auto finalOverlay = ManifestProposalOverlay::build(finalRequest, proposals_);
    if (!finalOverlay.success()) return fail<ManifestVerificationManualHandoffImportOutcome>(
        finalOverlay.error()->code, finalOverlay.error()->message);
    struct FinalCleanup {
        ManifestProposalOverlayDescriptor descriptor;
        ~FinalCleanup() { static_cast<void>(ManifestProposalOverlay::cleanup(descriptor)); }
    } finalCleanup {finalOverlay.value()};

    // Post-Stage-3 QML-lint hardening: a source-only baseline overlay over
    // the SAME already-captured baseline, built the same way this method's
    // own baseline verification above does. It lets every QML-lint
    // requirement tell unchanged pre-existing project debt from a genuinely
    // new finding instead of blocking on any warning anywhere in the module
    // - see qmlLintBaselineDeltaCheck().
    // Never widens allowedPaths, Manifest ownership, or AG-16 authority.
    BuildManifestProposalOverlayRequest finalBaselineOnlyRequest;
    finalBaselineOnlyRequest.workspaceRoot = workspaceRoot;
    finalBaselineOnlyRequest.trustedScratchRoot = ephemeralRoot.value();
    finalBaselineOnlyRequest.workspaceIdentity = handoff.workspaceIdentity;
    finalBaselineOnlyRequest.baselinePolicy = finalBaselinePolicy;
    finalBaselineOnlyRequest.baselineSnapshot = finalBaseline.value();
    auto finalBaselineOnlyOverlay = ManifestProposalOverlay::build(finalBaselineOnlyRequest, proposals_);
    if (!finalBaselineOnlyOverlay.success()) return fail<ManifestVerificationManualHandoffImportOutcome>(
        finalBaselineOnlyOverlay.error()->code, finalBaselineOnlyOverlay.error()->message);
    struct FinalBaselineOnlyCleanup {
        ManifestProposalOverlayDescriptor descriptor;
        ~FinalBaselineOnlyCleanup() { static_cast<void>(ManifestProposalOverlay::cleanup(descriptor)); }
    } finalBaselineOnlyCleanup {finalBaselineOnlyOverlay.value()};

    ManifestProposalSetVerifierRequest finalVerifierRequest;
    finalVerifierRequest.overlay = finalOverlay.value();
    finalVerifierRequest.tasks = manifest.value()->tasks;
    finalVerifierRequest.baselineOverlay = &finalBaselineOnlyOverlay.value();
    const auto finalVerification = ManifestProposalSetVerifier::verify(
        finalVerifierRequest, proposals_, commandRunner_, stopToken);
    if (!finalVerification.success()) return fail<ManifestVerificationManualHandoffImportOutcome>(
        finalVerification.error()->code, finalVerification.error()->message);

    const bool passed = requiredAcceptancePassed(finalVerification.value());
    const RunStatus finalStatus = passed ? RunStatus::Completed : RunStatus::Failed;

    ManifestWorkLedgerEntry ledgerEntry;
    ledgerEntry.runId = attemptRunId;
    ledgerEntry.projectId = handoff.sourceProjectId;
    ledgerEntry.terminalStatus = finalStatus;
    ledgerEntry.baselineFingerprint = handoff.baselineFingerprint;
    ledgerEntry.disposition = passed
        ? ManifestWorkLedgerDisposition::ManifestReady
        : ManifestWorkLedgerDisposition::NotProduced;
    // Synthetic task dispositions, one per amended proposal - lets
    // ManifestApplyVerificationGate's manual-handoff eligibility path reuse
    // the exact same "own-Run-member must be a recorded final proposal"
    // shape the continuation-child path already uses, never a bespoke
    // second rule.
    for (const auto& [ownerProposalId, successorId] : successorByOriginal) {
        const auto ownerTaskId = std::find_if(
            handoff.allowedPaths.begin(), handoff.allowedPaths.end(),
            [&](const ManifestVerificationManualHandoffAllowedPath& allowed) {
                return allowed.ownerProposalId == ownerProposalId;
            });
        ledgerEntry.taskDispositions.push_back({
            ownerTaskId != handoff.allowedPaths.end() ? ownerTaskId->ownerTaskId : ownerProposalId,
            "succeeded", successorId, std::nullopt});
    }
    ledgerEntry.proposalIds = finalProposalIds;
    ledgerEntry.verification = finalVerification.value();
    ledgerEntry.createdAt = attemptStartedAt;
    const auto insertedLedger = workLedger_.insertEntry(ledgerEntry);
    if (!insertedLedger.success()) return fail<ManifestVerificationManualHandoffImportOutcome>(
        insertedLedger.error()->code, insertedLedger.error()->message);

    // The ledger is the authoritative terminal result. From this point a
    // crash cannot lose verification: replay reconciles the Run/stage from
    // this immutable entry. Do not let the failure guard overwrite it.
    incompleteAttempt.dismiss();

    stage.status = passed ? StageStatus::Completed : StageStatus::Failed;
    stage.progress = passed ? 1.0 : 0.0;
    stage.finishedAt = std::chrono::system_clock::now();
    stage.errorMessage = passed ? std::string{} : "Manual handoff verification did not pass";
    const auto updatedStage = runs_.updateStage(stage);
    if (!updatedStage.success()) return fail<ManifestVerificationManualHandoffImportOutcome>(
        updatedStage.error()->code, updatedStage.error()->message);

    attemptRun.status = finalStatus;
    attemptRun.progress = passed ? 1.0 : 0.0;
    attemptRun.currentStageId.reset();
    attemptRun.finishedAt = stage.finishedAt;
    attemptRun.errorMessage = passed ? std::string{} : "Manual handoff verification did not pass";
    const auto updatedRun = runs_.updateRun(attemptRun);
    if (!updatedRun.success()) return fail<ManifestVerificationManualHandoffImportOutcome>(
        updatedRun.error()->code, updatedRun.error()->message);

    // QML-P4R.3.4: the durable, Backend-owned typed classification of this
    // attempt's outcome - VerifiedSuccess or VerifiedContentFailure,
    // decided purely from `passed`, the SAME boolean that decided the
    // ledger's own disposition above, never from any error text. Written
    // after the ledger (already the ultimate authority) and after the
    // Run/stage terminal update, matching the independent, order-tolerant
    // self-heal discipline reconcileInterruptedAttempts and this same
    // method's own replay branch both already rely on if a crash lands
    // before this specific step commits.
    const auto typedResult = ensureVerifiedAttemptResult(attempt.id, attemptRunId, finalStatus);
    if (!typedResult.success()) return fail<ManifestVerificationManualHandoffImportOutcome>(
        typedResult.error()->code, typedResult.error()->message);

    proposals_.releaseRunClaims(attemptRunId);

    return common::Result<ManifestVerificationManualHandoffImportOutcome>::ok(
        {attemptRunId, finalStatus, finalVerification.value(), /*idempotentReplay=*/false});
}

common::Result<ManifestVerificationManualHandoffResetResult>
ManifestVerificationManualHandoffService::resetNonEditableChanges(
    const std::string& handoffId, std::stop_token stopToken)
{
    if (stopToken.stop_requested()) {
        return fail<ManifestVerificationManualHandoffResetResult>(
            "manual_handoff.reset_cancelled", "Candidate reset was cancelled before it started");
    }
    std::lock_guard importLock(importMutex_);
    const auto handoffResult = manualHandoffs_.findHandoffById(handoffId);
    if (!handoffResult.success()) return fail<ManifestVerificationManualHandoffResetResult>(
        handoffResult.error()->code, handoffResult.error()->message);
    if (!handoffResult.value().has_value()) {
        return fail<ManifestVerificationManualHandoffResetResult>(
            "manual_handoff.not_found", "No manual handoff exists with this id");
    }
    const ManifestVerificationManualHandoff handoff = *handoffResult.value();

    const auto attempts = manualHandoffs_.listAttemptsForHandoff(handoff.id);
    if (!attempts.success()) return fail<ManifestVerificationManualHandoffResetResult>(
        attempts.error()->code, attempts.error()->message);
    for (const auto& attempt : attempts.value()) {
        const auto attemptRun = runs_.findRun(attempt.attemptRunId);
        if (!attemptRun.success()) return fail<ManifestVerificationManualHandoffResetResult>(
            attemptRun.error()->code, attemptRun.error()->message);
        if (attemptRun.value().has_value() && isActiveAttemptStatus(attemptRun.value()->status)) {
            return fail<ManifestVerificationManualHandoffResetResult>(
                "manual_handoff.reset_attempt_active",
                "A reverify attempt against this handoff is still in progress");
        }
    }

    const auto candidateRoot = resolveCandidateRoot(handoff);
    if (!candidateRoot.success()) return fail<ManifestVerificationManualHandoffResetResult>(
        candidateRoot.error()->code, candidateRoot.error()->message);
    const auto candidatePolicy = WorkspaceReadPolicy::secureDefaults(candidateRoot.value());
    const auto firstLive = WorkspaceBaselineProvenance::capture(candidatePolicy);
    const auto secondLive = WorkspaceBaselineProvenance::capture(candidatePolicy);
    if (!firstLive.success() || !secondLive.success()
        || firstLive.value().fingerprint != secondLive.value().fingerprint) {
        return fail<ManifestVerificationManualHandoffResetResult>(
            "manual_handoff.workspace_unstable",
            "The candidate directory could not be captured stably for reset");
    }
    const auto inspection = inspectCandidateEdits(handoff);
    if (!inspection.success()) return fail<ManifestVerificationManualHandoffResetResult>(
        inspection.error()->code, inspection.error()->message);
    if (!inspection.value().candidate.liveFingerprint.has_value()
        || *inspection.value().candidate.liveFingerprint != firstLive.value().fingerprint) {
        return fail<ManifestVerificationManualHandoffResetResult>(
            "manual_handoff.workspace_unstable",
            "The candidate directory changed while reset was being prepared");
    }
    if (inspection.value().resetRequiredPaths.empty()) {
        return fail<ManifestVerificationManualHandoffResetResult>(
            "manual_handoff.no_non_editable_changes",
            "The candidate contains no outside-scope or unsupported changes to reset");
    }

    const auto project = projects_.findById(handoff.sourceProjectId);
    if (!project.success()) return fail<ManifestVerificationManualHandoffResetResult>(
        project.error()->code, project.error()->message);
    if (!project.value().has_value()) return fail<ManifestVerificationManualHandoffResetResult>(
        "manual_handoff.source_project_missing", "The source project does not exist");
    const auto sourceRun = runs_.findRun(handoff.originManifestRunId);
    if (!sourceRun.success()) return fail<ManifestVerificationManualHandoffResetResult>(
        sourceRun.error()->code, sourceRun.error()->message);
    if (!sourceRun.value().has_value()) return fail<ManifestVerificationManualHandoffResetResult>(
        "manual_handoff.source_missing", "The origin Manifest Run does not exist");
    const auto workspaceRoot =
        sourceRun.value()->workspacePath.value_or(project.value()->rootPath);
    // The validated root of the temporary verification copies: refused here, before the workspace is captured or copied.
    const auto ephemeralRoot = resolveVerificationScratchRoot(scratchProvider_, ephemeralScratchRoot_, workspaceRoot);
    if (!ephemeralRoot.success()) return fail<ManifestVerificationManualHandoffResetResult>(
        ephemeralRoot.error()->code, ephemeralRoot.error()->message);
    const auto baselinePolicy = WorkspaceReadPolicy::secureDefaults(workspaceRoot);
    const auto baseline = WorkspaceBaselineProvenance::capture(baselinePolicy);
    if (!baseline.success()) return fail<ManifestVerificationManualHandoffResetResult>(
        baseline.error()->code, baseline.error()->message);
    if (baseline.value().fingerprint != handoff.baselineFingerprint) {
        return fail<ManifestVerificationManualHandoffResetResult>(
            "manual_handoff.baseline_drift",
            "The live workspace no longer matches the handoff's frozen baseline; reset refuses "
            "an implicit rebase");
    }

    BuildManifestProposalOverlayRequest pristineRequest;
    pristineRequest.workspaceRoot = workspaceRoot;
    pristineRequest.trustedScratchRoot = ephemeralRoot.value();
    pristineRequest.workspaceIdentity = handoff.workspaceIdentity;
    pristineRequest.baselinePolicy = baselinePolicy;
    pristineRequest.baselineSnapshot = baseline.value();
    std::vector<std::string> expectedContentHashes;
    for (const auto& member : handoff.frozenMembers) {
        pristineRequest.orderedProposalIds.push_back(member.proposalId);
        expectedContentHashes.push_back(member.contentHash);
    }
    auto pristine = ManifestProposalOverlay::build(pristineRequest, proposals_);
    if (!pristine.success()) return fail<ManifestVerificationManualHandoffResetResult>(
        pristine.error()->code, pristine.error()->message);
    CandidateOverlayCleanup pristineCleanup {pristine.value(), false};
    if (pristine.value().orderedProposalContentHashes != expectedContentHashes
        || pristine.value().overlayFingerprint != handoff.sourceOverlayFingerprint) {
        return fail<ManifestVerificationManualHandoffResetResult>(
            "manual_handoff.reset_source_mismatch",
            "The reconstructed frozen candidate no longer matches the handoff identity");
    }
    const auto pristineSnapshot = WorkspaceBaselineProvenance::capture(
        WorkspaceReadPolicy::secureDefaults(pristine.value().root));
    if (!pristineSnapshot.success()
        || pristineSnapshot.value().fingerprint != handoff.originalSnapshot.fingerprint) {
        return fail<ManifestVerificationManualHandoffResetResult>(
            "manual_handoff.reset_source_mismatch",
            "The reconstructed frozen candidate does not match the retained original snapshot");
    }
    if (stopToken.stop_requested()) {
        return fail<ManifestVerificationManualHandoffResetResult>(
            "manual_handoff.reset_cancelled", "Candidate reset was cancelled before mutation");
    }

    std::unordered_map<std::string, WorkspaceBaselineEntry> originalByPath;
    for (const auto& file : handoff.originalSnapshot.files) {
        originalByPath.emplace(
            detail::normalizedPathKey(detail::pathFromUtf8(file.relativePath)), file);
    }
    std::unordered_map<std::string, WorkspaceBaselineEntry> beforeByPath;
    for (const auto& file : firstLive.value().files) {
        beforeByPath.emplace(
            detail::normalizedPathKey(detail::pathFromUtf8(file.relativePath)), file);
    }

    // Preflight pass: resolve and validate every reset-required path BEFORE
    // any filesystem mutation begins. A multi-file reset cannot be made
    // transactional, but resolving every target/source and confirming the
    // exact filesystem shape (removable regular file for a new path,
    // available frozen source for a restored one) up front means a single
    // bad path is caught before any file is touched, rather than after a
    // partial reset has already happened.
    struct PlannedReset {
        std::string relativePath;
        std::filesystem::path target;
        std::optional<std::string> sourceRelativePath;
        std::optional<std::filesystem::path> source; // unset -> remove target
    };
    std::vector<PlannedReset> plan;
    plan.reserve(inspection.value().resetRequiredPaths.size());
    for (const auto& relativePath : inspection.value().resetRequiredPaths) {
        const auto key = detail::normalizedPathKey(detail::pathFromUtf8(relativePath));
        const auto target = resolveResetPath(candidateRoot.value(), relativePath);
        if (!target.success()) return fail<ManifestVerificationManualHandoffResetResult>(
            target.error()->code, target.error()->message);
        const auto original = originalByPath.find(key);
        if (original == originalByPath.end()) {
            std::error_code statusError;
            const auto status = std::filesystem::symlink_status(target.value(), statusError);
            if (statusError || !std::filesystem::is_regular_file(status)) {
                return fail<ManifestVerificationManualHandoffResetResult>(
                    "manual_handoff.reset_failed",
                    "A newly-added candidate path is no longer a removable regular file");
            }
            plan.push_back({relativePath, target.value(), std::nullopt, std::nullopt});
        } else {
            const auto source = resolveResetPath(pristine.value().root, original->second.relativePath);
            if (!source.success()) return fail<ManifestVerificationManualHandoffResetResult>(
                source.error()->code, source.error()->message);
            std::error_code sourceError;
            if (!std::filesystem::is_regular_file(source.value(), sourceError) || sourceError) {
                return fail<ManifestVerificationManualHandoffResetResult>(
                    "manual_handoff.reset_source_mismatch",
                    "A frozen candidate file is unavailable during reset");
            }
            plan.push_back(
                {relativePath, target.value(), original->second.relativePath, source.value()});
        }
    }

    // Execute pass. From this point cancellation no longer changes the
    // outcome: individual filesystem replacements cannot be rolled back
    // transactionally. Finish the bounded reset set, then verify the
    // resulting snapshot honestly - a failure partway through is reported
    // as the honest typed error it is, never as a claimed rollback.
    for (const auto& step : plan) {
        // The candidate is intentionally user-editable. Repeat the reparse/
        // containment resolution and shape checks at the mutation boundary;
        // a safe preflight path must not be trusted after an external editor
        // had an opportunity to replace a parent with a junction or the leaf
        // with a symlink.
        const auto currentTarget = resolveResetPath(candidateRoot.value(), step.relativePath);
        if (!currentTarget.success() || currentTarget.value() != step.target) {
            return fail<ManifestVerificationManualHandoffResetResult>(
                currentTarget.success() ? "manual_handoff.reset_path_invalid"
                                        : currentTarget.error()->code,
                currentTarget.success()
                    ? "A candidate reset path changed after preflight"
                    : currentTarget.error()->message);
        }
        if (!step.source.has_value()) {
            std::error_code statusError;
            const auto status = std::filesystem::symlink_status(currentTarget.value(), statusError);
            if (statusError || !std::filesystem::is_regular_file(status)) {
                return fail<ManifestVerificationManualHandoffResetResult>(
                    "manual_handoff.reset_failed",
                    "A newly-added candidate path changed after reset preflight");
            }
            std::error_code removeError;
            if (!std::filesystem::remove(currentTarget.value(), removeError) || removeError) {
                return fail<ManifestVerificationManualHandoffResetResult>(
                    "manual_handoff.reset_failed",
                    "A newly-added candidate file could not be removed");
            }
        } else {
            const auto currentSource = resolveResetPath(
                pristine.value().root, *step.sourceRelativePath);
            if (!currentSource.success() || currentSource.value() != *step.source) {
                return fail<ManifestVerificationManualHandoffResetResult>(
                    "manual_handoff.reset_source_mismatch",
                    "A frozen candidate source changed after reset preflight");
            }
            const auto restored = replaceCandidateFile(currentSource.value(), currentTarget.value());
            if (!restored.success()) return fail<ManifestVerificationManualHandoffResetResult>(
                restored.error()->code, restored.error()->message);
        }
    }

    const auto after = WorkspaceBaselineProvenance::capture(candidatePolicy);
    if (!after.success()) return fail<ManifestVerificationManualHandoffResetResult>(
        "manual_handoff.reset_verification_failed",
        "The candidate could not be captured after reset");
    const auto afterInspection = inspectCandidateEdits(handoff);
    if (!afterInspection.success()
        || !afterInspection.value().candidate.liveFingerprint.has_value()
        || *afterInspection.value().candidate.liveFingerprint != after.value().fingerprint
        || !afterInspection.value().resetRequiredPaths.empty()) {
        return fail<ManifestVerificationManualHandoffResetResult>(
            "manual_handoff.reset_verification_failed",
            "The candidate changed during post-reset verification or outside-scope changes remain");
    }
    std::unordered_map<std::string, WorkspaceBaselineEntry> afterByPath;
    for (const auto& file : after.value().files) {
        afterByPath.emplace(
            detail::normalizedPathKey(detail::pathFromUtf8(file.relativePath)), file);
    }
    for (const auto& path : inspection.value().editableChangedPaths) {
        const auto key = detail::normalizedPathKey(detail::pathFromUtf8(path));
        if (!beforeByPath.contains(key) || !afterByPath.contains(key)
            || beforeByPath.at(key).sha256 != afterByPath.at(key).sha256) {
            return fail<ManifestVerificationManualHandoffResetResult>(
                "manual_handoff.reset_verification_failed",
                "An authorized candidate edit changed while non-editable paths were reset");
        }
    }

    return common::Result<ManifestVerificationManualHandoffResetResult>::ok({
        inspection.value().resetRequiredPaths,
        inspection.value().editableChangedPaths,
        after.value().fingerprint});
}

common::Result<void> ManifestVerificationManualHandoffService::discardHandoff(
    const std::string& handoffId, std::stop_token stopToken)
{
    // The anchor is read BEFORE the delete: afterwards the handoff row is gone.
    OutcomeBriefNudge nudge(outcomeBriefTrigger_);
    {
        const auto existing = manualHandoffs_.findHandoffById(handoffId);
        if (existing.success() && existing.value().has_value()) nudge.addAnchor(existing.value()->verificationAnchorRunId);
    }
    if (stopToken.stop_requested()) {
        return fail<void>("manual_handoff.discard_cancelled",
            "Manual-handoff discard was cancelled before the database delete");
    }
    // Same mutex importAndReverify() takes - see this method's own doc
    // comment on why discard must be serialized against a concurrent
    // reverify rather than merely against other discards.
    std::lock_guard importLock(importMutex_);
    DrainDiscardedAnchors drainDiscarded(discardedAnchorsLocked_, nudge);
    if (stopToken.stop_requested()) {
        return fail<void>("manual_handoff.discard_cancelled",
            "Manual-handoff discard was cancelled before the database delete");
    }
    return discardHandoffLocked(handoffId, stopToken);
}

// QML-P4R.3.5: extracted so retention eviction (already holding
// importMutex_ for its own count-then-evict loop) can reuse this exact
// DB-first-then-filesystem-best-effort mechanics without a self-deadlock
// on the non-recursive importMutex_. Never called without the lock
// already held - see this class's own private declaration comment.
common::Result<void> ManifestVerificationManualHandoffService::discardHandoffLocked(
    const std::string& handoffId, std::stop_token stopToken)
{
    if (stopToken.stop_requested()) {
        return fail<void>("manual_handoff.discard_cancelled",
            "Manual-handoff discard was cancelled before the database delete");
    }
    const auto handoffResult = manualHandoffs_.findHandoffById(handoffId);
    if (!handoffResult.success()) return common::Result<void>::failure(*handoffResult.error());
    if (!handoffResult.value().has_value()) {
        return fail<void>("manual_handoff.not_found", "No manual handoff exists with this id");
    }
    const ManifestVerificationManualHandoff handoff = *handoffResult.value();

    const auto attempts = manualHandoffs_.listAttemptsForHandoff(handoffId);
    if (!attempts.success()) return common::Result<void>::failure(*attempts.error());
    for (const auto& attempt : attempts.value()) {
        const auto attemptRun = runs_.findRun(attempt.attemptRunId);
        if (!attemptRun.success()) return common::Result<void>::failure(*attemptRun.error());
        if (attemptRun.value().has_value()
            && isActiveAttemptStatus(attemptRun.value()->status)) {
            return fail<void>("manual_handoff.discard_attempt_active",
                "A reverify attempt against this handoff is still in progress");
        }
    }

    // The database row - and everything it cascades to - is the sole
    // authority for Apply eligibility and the frozen members' proposal-id
    // claim. It is deleted FIRST and unconditionally; only once that commits
    // does the filesystem candidate directory get removed, best-effort. See
    // this method's own header doc comment for why this order is the only
    // safe one.
    if (stopToken.stop_requested()) {
        return fail<void>("manual_handoff.discard_cancelled",
            "Manual-handoff discard was cancelled before the database delete");
    }
    const auto discarded = manualHandoffs_.discardHandoff(handoffId);
    if (!discarded.success()) return common::Result<void>::failure(*discarded.error());
    // PC-4C: the row is gone, so the brief of its anchor changes; the command that holds the lock nudges it after the lock is released.
    discardedAnchorsLocked_.push_back(handoff.verificationAnchorRunId);

    const auto candidateRoot = resolveCandidateRoot(handoff);
    if (candidateRoot.success()) {
        std::error_code removeError;
        static_cast<void>(std::filesystem::remove_all(candidateRoot.value(), removeError));
        // A removal failure here (permission, a file still open elsewhere)
        // never undoes the already-committed database discard - the
        // directory becomes a safe, unreferenced orphan for a future
        // startup/retention pass to sweep, exactly as designed.
    }
    return common::Result<void>::ok();
}

// QML-P4R.3.5: never derived from any error text - purely from durable
// state (age, Run status, typed-result kind), assumes importMutex_ is
// already held.
common::Result<bool> ManifestVerificationManualHandoffService::isHandoffRetentionProtectedLocked(
    const ManifestVerificationManualHandoff& handoff)
{
    const auto age = std::chrono::system_clock::now() - handoff.createdAt;
    if (age < kManifestVerificationManualHandoffMinRetentionAge) {
        return common::Result<bool>::ok(true);
    }
    const auto attempts = manualHandoffs_.listAttemptsForHandoff(handoff.id);
    if (!attempts.success()) return common::Result<bool>::failure(*attempts.error());
    for (const auto& attempt : attempts.value()) {
        const auto attemptRun = runs_.findRun(attempt.attemptRunId);
        if (!attemptRun.success()) return common::Result<bool>::failure(*attemptRun.error());
        if (attemptRun.value().has_value()
                && isActiveAttemptStatus(attemptRun.value()->status)) {
            return common::Result<bool>::ok(true);
        }
        const auto typedResult = manualHandoffs_.findAttemptResultByAttemptId(attempt.id);
        if (!typedResult.success()) return common::Result<bool>::failure(*typedResult.error());
        const auto ledgerEntry = workLedger_.findEntry(attempt.attemptRunId);
        if (!ledgerEntry.success()) return common::Result<bool>::failure(*ledgerEntry.error());

        if (ledgerEntry.value().has_value()) {
            if (ledgerEntry.value()->runId != attempt.attemptRunId) {
                return fail<bool>("manual_handoff.attempt_result_conflict",
                    "This attempt's Work Ledger identity does not match the attempt Run");
            }
            const auto expectedKind = ledgerEntry.value()->terminalStatus == RunStatus::Completed
                ? ManifestVerificationManualHandoffAttemptResultKind::VerifiedSuccess
                : ManifestVerificationManualHandoffAttemptResultKind::VerifiedContentFailure;
            if (typedResult.value().has_value()) {
                const auto agrees = checkAttemptResultAgrees(*typedResult.value(), attempt.id,
                    attempt.attemptRunId, expectedKind);
                if (!agrees.success()) return common::Result<bool>::failure(*agrees.error());
            }
            // The Work Ledger is terminal authority. In particular, protect
            // a successful attempt even if a crash committed its ledger just
            // before the typed-result projection and reconciliation has not
            // yet been able to self-heal that projection.
            if (ledgerEntry.value()->terminalStatus == RunStatus::Completed) {
                return common::Result<bool>::ok(true);
            }
            continue;
        }

        if (typedResult.value().has_value()) {
            const auto kind = typedResult.value()->kind;
            if (kind == ManifestVerificationManualHandoffAttemptResultKind::VerifiedSuccess
                || kind == ManifestVerificationManualHandoffAttemptResultKind::VerifiedContentFailure) {
                return fail<bool>("manual_handoff.attempt_result_conflict",
                    "A verified typed result has no authoritative Work Ledger entry");
            }
            const auto agrees = checkAttemptResultAgrees(
                *typedResult.value(), attempt.id, attempt.attemptRunId, kind);
            if (!agrees.success()) return common::Result<bool>::failure(*agrees.error());
        }
    }
    return common::Result<bool>::ok(false);
}

common::Result<std::optional<std::string>>
ManifestVerificationManualHandoffService::evictOldestEligibleHandoffLocked()
{
    const auto handoffs = manualHandoffs_.listAllHandoffs();
    if (!handoffs.success()) return common::Result<std::optional<std::string>>::failure(
        *handoffs.error());
    std::vector<ManifestVerificationManualHandoff> ordered = handoffs.value();
    std::sort(ordered.begin(), ordered.end(),
        [](const ManifestVerificationManualHandoff& left,
            const ManifestVerificationManualHandoff& right) {
            if (left.createdAt != right.createdAt) return left.createdAt < right.createdAt;
            return left.id < right.id;
        });
    for (const auto& handoff : ordered) {
        const auto isProtected = isHandoffRetentionProtectedLocked(handoff);
        if (!isProtected.success()) return common::Result<std::optional<std::string>>::failure(
            *isProtected.error());
        if (isProtected.value()) continue;
        const auto discarded = discardHandoffLocked(handoff.id);
        if (!discarded.success()) return common::Result<std::optional<std::string>>::failure(
            *discarded.error());
        return common::Result<std::optional<std::string>>::ok(handoff.id);
    }
    return common::Result<std::optional<std::string>>::ok(std::nullopt);
}

common::Result<std::vector<std::string>>
ManifestVerificationManualHandoffService::sweepOrphanCandidateDirectoriesLocked()
{
    std::vector<std::string> swept;
    std::error_code rootError;
    if (!std::filesystem::is_directory(manualHandoffRoot_, rootError) || rootError) {
        // Nothing has ever been materialized into this root yet - a
        // perfectly normal, harmless state, never an error.
        return common::Result<std::vector<std::string>>::ok(std::move(swept));
    }
    std::error_code rootReparseError;
    if (isReparsePoint(manualHandoffRoot_, rootReparseError) || rootReparseError) {
        return fail<std::vector<std::string>>("manual_handoff.retention_sweep_failed",
            "The durable manual-handoff root is not a safe physical directory");
    }
    const auto handoffs = manualHandoffs_.listAllHandoffs();
    if (!handoffs.success()) return common::Result<std::vector<std::string>>::failure(
        *handoffs.error());
    std::unordered_set<std::string> liveStorageKeys;
    for (const auto& handoff : handoffs.value()) liveStorageKeys.insert(handoff.storageKey);

    std::error_code iterateError;
    std::filesystem::directory_iterator iterator(manualHandoffRoot_, iterateError);
    const std::filesystem::directory_iterator end;
    if (iterateError) {
        return common::Result<std::vector<std::string>>::failure({
            "manual_handoff.retention_sweep_failed",
            "The durable manual-handoff root could not be enumerated"});
    }
    while (iterator != end) {
        const auto entryPath = iterator->path();
        iterator.increment(iterateError);
        if (iterateError) {
            return common::Result<std::vector<std::string>>::failure({
                "manual_handoff.retention_sweep_failed",
                "The durable manual-handoff root could not be enumerated"});
        }
        // Never follow, and never act on, a symlink/reparse point -
        // symlink_status (which never follows it, unlike status()) is the
        // only safe check. Left completely untouched either way, never
        // removed - a sweep is not the place to make a safety judgment
        // about content a symlink might point at.
        std::error_code typeError;
        if (isReparsePoint(entryPath, typeError)) continue;
        if (typeError) {
            return fail<std::vector<std::string>>("manual_handoff.retention_sweep_failed",
                "A manual-handoff storage entry could not be inspected safely");
        }
        const auto entryStatus = std::filesystem::symlink_status(entryPath, typeError);
        if (typeError || entryStatus.type() != std::filesystem::file_type::directory) continue;
        const std::string name = entryPath.filename().string();
        if (liveStorageKeys.contains(name)) continue;
        const auto unsafeTree = containsReparsePoint(entryPath);
        if (!unsafeTree.success()) return common::Result<std::vector<std::string>>::failure(
            *unsafeTree.error());
        if (unsafeTree.value()) continue;
        std::error_code removeError;
        std::filesystem::remove_all(entryPath, removeError);
        if (!removeError) swept.push_back(name);
        // A removal failure here is best-effort, like discardHandoff()'s
        // own filesystem step - the directory remains a safe orphan for a
        // later sweep to retry, never a correctness issue.
    }
    return common::Result<std::vector<std::string>>::ok(std::move(swept));
}

common::Result<void> ManifestVerificationManualHandoffService::ensureRetentionCapacityForNewHandoffLocked()
{
    const auto handoffs = manualHandoffs_.listAllHandoffs();
    if (!handoffs.success()) return common::Result<void>::failure(*handoffs.error());
    if (handoffs.value().size() < kManifestVerificationManualHandoffMaxRetainedHandoffs) {
        return common::Result<void>::ok();
    }

    std::vector<ManifestVerificationManualHandoff> ordered = handoffs.value();
    std::sort(ordered.begin(), ordered.end(),
        [](const ManifestVerificationManualHandoff& left,
            const ManifestVerificationManualHandoff& right) {
            if (left.createdAt != right.createdAt) return left.createdAt < right.createdAt;
            return left.id < right.id;
        });
    std::vector<std::string> eligibleIds;
    for (const auto& handoff : ordered) {
        const auto isProtected = isHandoffRetentionProtectedLocked(handoff);
        if (!isProtected.success()) return common::Result<void>::failure(*isProtected.error());
        if (!isProtected.value()) eligibleIds.push_back(handoff.id);
    }
    const std::size_t requiredEvictions = handoffs.value().size()
        - kManifestVerificationManualHandoffMaxRetainedHandoffs + 1;
    if (eligibleIds.size() < requiredEvictions) {
        return common::Result<void>::failure({
            "manual_handoff.retention_capacity_exhausted",
            "The manual-handoff retention capacity is full and every retained handoff is "
            "protected or too few retained handoffs are eligible for eviction (active, carrying "
            "Apply-authority, or too recently created) - refusing "
            "to materialize a new one"});
    }
    // Preflight the entire batch before deleting anything. If there are not
    // enough eligible rows to make room, the refusal above is mutation-free
    // instead of silently evicting a partial batch and then failing anyway.
    for (std::size_t i = 0; i < requiredEvictions; ++i) {
        const auto discarded = discardHandoffLocked(eligibleIds[i]);
        if (!discarded.success()) return common::Result<void>::failure(*discarded.error());
    }
    return common::Result<void>::ok();
}

common::Result<ManifestVerificationManualHandoffRetentionResult>
ManifestVerificationManualHandoffService::enforceRetention()
{
    OutcomeBriefNudge nudge(outcomeBriefTrigger_);   // PC-4C: the anchors of the evicted handoffs, once importMutex_ is released
    std::lock_guard importLock(importMutex_);
    DrainDiscardedAnchors drainEvicted(discardedAnchorsLocked_, nudge);
    ManifestVerificationManualHandoffRetentionResult result;
    for (;;) {
        const auto handoffs = manualHandoffs_.listAllHandoffs();
        if (!handoffs.success()) return fail<ManifestVerificationManualHandoffRetentionResult>(
            handoffs.error()->code, handoffs.error()->message);
        if (handoffs.value().size() <= kManifestVerificationManualHandoffMaxRetainedHandoffs) break;
        const auto evicted = evictOldestEligibleHandoffLocked();
        if (!evicted.success()) return fail<ManifestVerificationManualHandoffRetentionResult>(
            evicted.error()->code, evicted.error()->message);
        if (!evicted.value().has_value()) {
            // Best-effort: every remaining handoff is protected. Never a
            // failure here - only materialize()'s own capacity gate fails
            // closed, for a genuinely new candidate with nowhere to go.
            break;
        }
        result.evictedHandoffIds.push_back(*evicted.value());
    }
    const auto swept = sweepOrphanCandidateDirectoriesLocked();
    if (!swept.success()) return fail<ManifestVerificationManualHandoffRetentionResult>(
        swept.error()->code, swept.error()->message);
    result.sweptOrphanDirectoryNames = swept.value();
    return common::Result<ManifestVerificationManualHandoffRetentionResult>::ok(std::move(result));
}

common::Result<ManifestVerificationManualHandoffReconciliationResult>
ManifestVerificationManualHandoffService::reconcileInterruptedAttempts()
{
    const auto attempts = manualHandoffs_.listAllAttempts();
    if (!attempts.success()) return fail<ManifestVerificationManualHandoffReconciliationResult>(
        attempts.error()->code, attempts.error()->message);

    ManifestVerificationManualHandoffReconciliationResult result;
    for (const auto& attempt : attempts.value()) {
        const auto run = runs_.findRun(attempt.attemptRunId);
        if (!run.success()) return fail<ManifestVerificationManualHandoffReconciliationResult>(
            run.error()->code, run.error()->message);
        if (!run.value().has_value()) {
            // Crash window: reserved via insertAttempt() but the process
            // died before insertRunWithStages() ever ran. Nothing
            // downstream (Run, stage, successor proposal, claim) could
            // possibly exist yet - there is nothing to repair. This row
            // becomes permanently harmless dead audit history:
            // findAttemptByFingerprint's own "no Run" branch already lets a
            // later importAndReverify() call for the same edited state
            // allocate a fresh ordinal instead of ever blocking on it.
            continue;
        }
        if (isActiveAttemptStatus(run.value()->status)) {
            // Should never happen once recoverInterruptedRuns() has already
            // turned every non-Prepared unfinished Run into Interrupted
            // before this call runs - but never assumed. A genuinely still-
            // active attempt is left completely alone.
            continue;
        }

        const auto entry = workLedger_.findEntry(attempt.attemptRunId);
        if (!entry.success()) return fail<ManifestVerificationManualHandoffReconciliationResult>(
            entry.error()->code, entry.error()->message);

        if (entry.value().has_value()) {
            // Crash window: the authoritative Work Ledger entry committed,
            // but the process died before the Run/stage terminal update
            // that ordinarily follows it - exactly the gap
            // importAndReverify()'s own lazy self-heal branch already
            // repairs on a caller's NEXT call for this same handoff.
            // Applied here proactively at startup instead. Successor
            // proposals/claims are never touched by this branch - the
            // ledger is already the one authoritative record of what
            // became of them.
            //
            // QML-P4R.3.4 hardening: this attempt's own typed result is
            // validated (and, if absent, written) BEFORE the Run/stage
            // projection below is touched, never after - a conflict (an
            // existing typed result that disagrees with what this
            // ledger entry's own terminalStatus implies) must be caught
            // before any mutation, not discovered only once the
            // projection has already been rewritten to match it. A
            // genuine repository failure here is a hard startup error,
            // exactly like every other write in this loop, never a
            // silently-discarded one.
            const auto typedResultAgrees = ensureVerifiedAttemptResult(
                attempt.id, attempt.attemptRunId, entry.value()->terminalStatus);
            if (!typedResultAgrees.success()) return fail<ManifestVerificationManualHandoffReconciliationResult>(
                typedResultAgrees.error()->code, typedResultAgrees.error()->message);
            Run repairedRun = *run.value();
            if (repairedRun.status != entry.value()->terminalStatus) {
                repairedRun.status = entry.value()->terminalStatus;
                repairedRun.progress = repairedRun.status == RunStatus::Completed ? 1.0 : 0.0;
                repairedRun.currentStageId.reset();
                repairedRun.finishedAt = std::chrono::system_clock::now();
                repairedRun.errorMessage = repairedRun.status == RunStatus::Completed
                    ? std::string{} : "Manual handoff verification did not pass";
                const auto updated = runs_.updateRun(std::move(repairedRun));
                if (!updated.success()) return fail<ManifestVerificationManualHandoffReconciliationResult>(
                    updated.error()->code, updated.error()->message);
            }
            const auto stages = runs_.listStages(attempt.attemptRunId);
            if (!stages.success()) return fail<ManifestVerificationManualHandoffReconciliationResult>(
                stages.error()->code, stages.error()->message);
            const StageStatus expectedStage = entry.value()->terminalStatus == RunStatus::Completed
                ? StageStatus::Completed : StageStatus::Failed;
            for (auto stage : stages.value()) {
                if (stage.status == expectedStage) continue;
                stage.status = expectedStage;
                stage.progress = expectedStage == StageStatus::Completed ? 1.0 : 0.0;
                stage.finishedAt = std::chrono::system_clock::now();
                stage.errorMessage = expectedStage == StageStatus::Completed
                    ? std::string{} : "Manual handoff verification did not pass";
                const auto updatedStage = runs_.updateStage(std::move(stage));
                if (!updatedStage.success()) return fail<ManifestVerificationManualHandoffReconciliationResult>(
                    updatedStage.error()->code, updatedStage.error()->message);
            }
            proposals_.releaseRunClaims(attempt.attemptRunId);
            result.repairedFromLedgerRunIds.push_back(attempt.attemptRunId);
            continue;
        }

        // Crash window: no authoritative ledger, and the Run is no longer
        // active (Interrupted by recoverInterruptedRuns(), or any other
        // non-active status reached without ever producing a result).
        // Mirrors importAndReverify()'s own in-process ScopeExit crash
        // cleanup exactly, applied here for a crash that outlived the
        // process itself: every successor proposal created mid-attempt
        // (ChangeProposalService::listForRun - the only proposals a manual-
        // handoff attempt Run ever creates) is marked missing-evidence,
        // claims are released, and any stage still short of a terminal
        // status is brought to Failed. Never marked Cancelled - a startup
        // reconciliation pass has no stop_token/user-cancellation concept,
        // only ManifestVerificationManualHandoffService's own established
        // "did not reach an authoritative result" outcome.
        //
        // QML-P4R.3.4 hardening: an existing typed result is checked
        // BEFORE any of the destructive marking below, never discovered
        // only afterward. A VerifiedSuccess/VerifiedContentFailure row
        // already recorded for this attempt directly contradicts "no
        // ledger exists" - never silently reinterpreted as Incomplete and
        // never let to mark genuinely-verified successors missing-
        // evidence; fail closed instead. Incomplete or
        // RejectedBeforeVerification already recorded here is NOT a
        // conflict - it means a prior pass (this attempt's own in-process
        // ScopeExit, or an earlier reconciliation run) already classified
        // it identically; the marking below remains safe to (redundantly
        // but harmlessly) re-apply idempotently.
        const auto existingResult = manualHandoffs_.findAttemptResultByAttemptId(attempt.id);
        if (!existingResult.success()) return fail<ManifestVerificationManualHandoffReconciliationResult>(
            existingResult.error()->code, existingResult.error()->message);
        if (existingResult.value().has_value()) {
            const auto& existingRow = *existingResult.value();
            const bool agrees = existingRow.attemptRunId == attempt.attemptRunId
                && (existingRow.kind == ManifestVerificationManualHandoffAttemptResultKind::Incomplete
                    || existingRow.kind
                        == ManifestVerificationManualHandoffAttemptResultKind::RejectedBeforeVerification);
            if (!agrees) {
                return fail<ManifestVerificationManualHandoffReconciliationResult>(
                    "manual_handoff.attempt_result_conflict",
                    "This attempt already carries a verified typed result but no Work Ledger "
                    "entry exists for it - refusing to treat it as Incomplete and mark its "
                    "successors missing-evidence");
            }
        }

        const auto successors = proposals_.listForRun(attempt.attemptRunId);
        if (!successors.success()) return fail<ManifestVerificationManualHandoffReconciliationResult>(
            successors.error()->code, successors.error()->message);
        for (const auto& successor : successors.value()) {
            const auto marked = proposals_.markMissingEvidence(
                successor.changeSetId, "manual_handoff.attempt_incomplete",
                "The manual-handoff attempt ended before authoritative verification was stored");
            // Never silently discarded (unlike importAndReverify()'s own
            // in-process ScopeExit, which is a best-effort destructor-time
            // cleanup with no result channel to report through) - a
            // reconciliation pass MUST let its caller know an orphan
            // successor could not be annotated, so startup never reports
            // clean recovery while leaving unmarked evidence behind. See
            // this field's own doc comment for why.
            if (!marked.success()) {
                result.unmarkedMissingEvidenceProposalIds.push_back(successor.changeSetId);
            }
        }
        proposals_.releaseRunClaims(attempt.attemptRunId);

        const auto stages = runs_.listStages(attempt.attemptRunId);
        if (!stages.success()) return fail<ManifestVerificationManualHandoffReconciliationResult>(
            stages.error()->code, stages.error()->message);
        for (auto stage : stages.value()) {
            if (stage.status == StageStatus::Failed || stage.status == StageStatus::Cancelled
                || stage.status == StageStatus::Completed) continue;
            stage.status = StageStatus::Failed;
            stage.progress = 0.0;
            stage.finishedAt = std::chrono::system_clock::now();
            stage.errorMessage = "Manual handoff reverify did not reach an authoritative result";
            const auto updatedStage = runs_.updateStage(std::move(stage));
            if (!updatedStage.success()) return fail<ManifestVerificationManualHandoffReconciliationResult>(
                updatedStage.error()->code, updatedStage.error()->message);
        }

        if (run.value()->status != RunStatus::Failed) {
            Run failedRun = *run.value();
            failedRun.status = RunStatus::Failed;
            failedRun.progress = 0.0;
            failedRun.currentStageId.reset();
            failedRun.finishedAt = std::chrono::system_clock::now();
            failedRun.errorMessage = "Manual handoff reverify did not reach an authoritative result";
            const auto updatedRun = runs_.updateRun(std::move(failedRun));
            if (!updatedRun.success()) return fail<ManifestVerificationManualHandoffReconciliationResult>(
                updatedRun.error()->code, updatedRun.error()->message);
        }
        // QML-P4R.3.4: this attempt ended without ever producing a Work
        // Ledger entry. Only written if the early lookup above found
        // nothing yet - reconciliation has no in-process
        // pendingContentRejection signal to consult (it never survives a
        // crash), so a genuinely NEW discovery here can only ever be
        // classified Incomplete, never RejectedBeforeVerification; a row
        // already present (Incomplete or RejectedBeforeVerification, both
        // already accepted as non-conflicting above) is left exactly as
        // it is - this row is never updated in place. Recorded durably
        // (with a real Result channel, unlike importAndReverify()'s own
        // best-effort in-process ScopeExit) so a crash that outlived the
        // process still leaves the same typed classification and the
        // same retry-permitted semantics behind.
        if (!existingResult.value().has_value()) {
            ManifestVerificationManualHandoffAttemptResult typedResult;
            typedResult.id = "manual-handoff-attempt-result:" + attempt.id;
            typedResult.attemptId = attempt.id;
            typedResult.attemptRunId = attempt.attemptRunId;
            typedResult.kind = ManifestVerificationManualHandoffAttemptResultKind::Incomplete;
            typedResult.recordedAt = std::chrono::system_clock::now();
            const auto inserted = manualHandoffs_.insertAttemptResult(std::move(typedResult));
            if (!inserted.success()) return fail<ManifestVerificationManualHandoffReconciliationResult>(
                inserted.error()->code, inserted.error()->message);
        }
        result.incompleteAttemptRunIds.push_back(attempt.attemptRunId);
    }
    return common::Result<ManifestVerificationManualHandoffReconciliationResult>::ok(std::move(result));
}

} // namespace ai::application
