#include "ai/application/workspace/FilesystemPathUtils.hpp"
#include "ai/application/verification/ManifestProposalOverlay.hpp"
#include "ai/application/verification/ManifestProposalSetVerifier.hpp"
#include "ai/application/verification/ManifestVerificationEnvironmentPreflight.hpp"
#include "ai/application/ManifestVerificationRepairability.hpp"
#include "ai/application/verification/QmlLintDiagnosticPolicy.hpp"
#include "ai/application/workspace/WorkspaceBaselineProvenance.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <map>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

namespace {

namespace fs = std::filesystem;
using namespace ai::application;
namespace common = ai::common;

void require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

struct TempDir {
    fs::path path = fs::temp_directory_path()
        / ("aio-overlay-test-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    TempDir() { fs::create_directories(path); }
    ~TempDir() { std::error_code error; fs::remove_all(path, error); }
};

void write(const fs::path& path, std::string_view content)
{
    fs::create_directories(path.parent_path());
    std::ofstream stream(path, std::ios::binary);
    stream.write(content.data(), static_cast<std::streamsize>(content.size()));
}

std::string read(const fs::path& path)
{
    std::ifstream stream(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

class ProposalRepository final : public IChangeProposalRepository {
public:
    std::map<ChangeSetId, ProposedChangeSet> sets;
    std::map<ChangeSetId, std::vector<ProposedFileChange>> changes;

    common::Result<void> insertChangeSet(const ProposedChangeSet& value) override
    { sets[value.changeSetId] = value; changes[value.changeSetId] = value.changes; return common::Result<void>::ok(); }
    common::Result<ProposedChangeSet> updateChangeSetState(const ChangeSetId& id, ChangeSetState state) override
    { sets[id].state = state; return common::Result<ProposedChangeSet>::ok(sets[id]); }
    common::Result<void> markMissingEvidence(const ChangeSetId&, std::string, std::string) override
    { return common::Result<void>::ok(); }
    common::Result<std::optional<ProposedChangeSet>> findChangeSet(const ChangeSetId& id) const override
    { const auto it=sets.find(id); return common::Result<std::optional<ProposedChangeSet>>::ok(it==sets.end()?std::nullopt:std::optional<ProposedChangeSet>(it->second)); }
    common::Result<std::vector<ProposedChangeSet>> listForRun(const RunId&) const override
    { return common::Result<std::vector<ProposedChangeSet>>::ok({}); }
    common::Result<std::vector<ProposedChangeSet>> listForProject(const ProjectId&) const override
    { return common::Result<std::vector<ProposedChangeSet>>::ok({}); }
    common::Result<std::vector<ProposedFileChange>> listChanges(const ChangeSetId& id) const override
    { const auto it=changes.find(id); return common::Result<std::vector<ProposedFileChange>>::ok(it==changes.end()?std::vector<ProposedFileChange>{}:it->second); }
    common::Result<std::vector<ValidationFinding>> listValidationFindings(const ChangeSetId&) const override
    { return common::Result<std::vector<ValidationFinding>>::ok({}); }
    common::Result<std::unordered_map<ChangeSetId,std::vector<ProposedFileChange>>> listChangesForSets(const std::vector<ChangeSetId>& ids) const override
    { std::unordered_map<ChangeSetId,std::vector<ProposedFileChange>> result; for(const auto& id:ids){const auto it=changes.find(id);if(it!=changes.end())result[id]=it->second;} return common::Result<decltype(result)>::ok(std::move(result)); }
    common::Result<std::unordered_map<ChangeSetId,std::vector<ValidationFinding>>> listValidationFindingsForSets(const std::vector<ChangeSetId>&) const override
    { return common::Result<std::unordered_map<ChangeSetId,std::vector<ValidationFinding>>>::ok({}); }
};

class ProposalStore final : public IChangeProposalStore {
public:
    std::map<std::pair<std::string,ChangeId>,std::string> proposed;
    common::Result<PublishedProposalLocation> publishChangeSet(const ProposedChangeSet&, const ProjectWritePolicy&) override
    { return common::Result<PublishedProposalLocation>::failure({"test.unused","unused"}); }
    common::Result<std::string> readOriginalContent(const std::string&, const ChangeId&) const override
    { return common::Result<std::string>::failure({"test.unused","unused"}); }
    common::Result<std::string> readProposedContent(const std::string& root, const ChangeId& id) const override
    { const auto it=proposed.find({root,id}); return it==proposed.end()?common::Result<std::string>::failure(common::Error{"proposal.content_not_found","missing"}):common::Result<std::string>::ok(it->second); }
};

struct Fixture {
    TempDir workspace;
    TempDir scratch;
    ProposalRepository repository;
    ProposalStore store;
    ChangeProposalService service {repository, store};

    void add(const std::string& id, std::vector<ProposedFileChange> changes)
    {
        ProposedChangeSet set;
        set.changeSetId = id;
        set.state = ChangeSetState::ReadyForReview;
        set.artifactRelativeRoot = id;
        repository.sets[id] = set;
        repository.changes[id] = changes;
        for (const auto& change : changes) {
            if (change.proposedContent.has_value())
                store.proposed[{id, change.changeId}] = *change.proposedContent;
        }
    }
};

ProposedFileChange change(std::string id, FileChangeOperation operation,
    std::string path, std::optional<std::string> content = std::nullopt,
    std::optional<std::string> source = std::nullopt)
{
    ProposedFileChange value;
    value.changeId = std::move(id);
    value.operation = operation;
    value.relativePath = std::move(path);
    value.proposedContent = std::move(content);
    if (source.has_value()) value.sourcePath = *source;
    return value;
}

BuildManifestProposalOverlayRequest overlayRequest(const Fixture& fixture,
    std::vector<ChangeSetId> ids)
{
    BuildManifestProposalOverlayRequest request;
    request.workspaceRoot = fixture.workspace.path;
    request.trustedScratchRoot = fixture.scratch.path;
    request.workspaceIdentity = "workspace-1";
    request.baselinePolicy = WorkspaceReadPolicy::secureDefaults(
        fs::absolute(fixture.workspace.path));
    const auto baseline = WorkspaceBaselineProvenance::capture(
        request.baselinePolicy);
    require(baseline.success(), "Overlay fixture baseline capture failed");
    request.baselineSnapshot = baseline.value();
    request.orderedProposalIds = std::move(ids);
    return request;
}

class FakeCommandRunner final : public IManifestVerificationCommandRunner {
public:
    std::vector<ManifestVerificationCommandOutcome> outcomes;
    std::vector<ManifestVerificationCommandRequest> requests;
    std::size_t calls {0};
    ManifestVerificationCommandOutcome run(
        const ManifestVerificationCommandRequest& request, std::stop_token) override
    {
        requests.push_back(request);
        return calls < outcomes.size()
            ? outcomes[calls++] : ManifestVerificationCommandOutcome{};
    }
};

ExecutionManifestTask taskWithRequirement(ExecutionAcceptanceRequirement requirement)
{
    ExecutionManifestTask task;
    task.id = "verify";
    task.objective = "Verify combined candidate";
    task.role = "verifier";
    ExecutionAcceptanceContract contract;
    contract.requirements.push_back(std::move(requirement));
    task.acceptanceContract = std::move(contract);
    task.requestedCapability = WorkspaceCapability::Inspect;
    return task;
}

ManifestVerificationCommandOutcome successfulProbe(std::string output)
{
    ManifestVerificationCommandOutcome result;
    result.started = true;
    result.exitCode = 0;
    result.boundedOutput = std::move(output);
    return result;
}

ManifestVerificationEnvironmentPreflightRequest preflightRequest(
    const fs::path& root, ExecutionAcceptanceRequirement requirement)
{
    ManifestVerificationEnvironmentPreflightRequest request;
    request.workspaceRoot = root;
    request.tasks = {taskWithRequirement(std::move(requirement))};
    return request;
}

void testOverlayMaterializesAllOperationsAndPreservesWorkspace()
{
    Fixture fixture;
    write(fixture.workspace.path / "modify.txt", "old");
    write(fixture.workspace.path / "delete.txt", "delete");
    write(fixture.workspace.path / "rename-source.txt", "rename");
    write(fixture.workspace.path / ".git" / "config", "must-not-copy");
    fixture.add("p1", {
        change("create", FileChangeOperation::Create, "new/create.txt", "created"),
        change("modify", FileChangeOperation::Modify, "modify.txt", "new"),
        change("delete", FileChangeOperation::Delete, "delete.txt"),
        change("rename", FileChangeOperation::Rename, "renamed.txt", std::nullopt, "rename-source.txt")});

    const auto overlay = ManifestProposalOverlay::build(
        overlayRequest(fixture, {"p1"}), fixture.service);
    require(overlay.success(), "All-operation overlay did not build");
    require(read(overlay.value().root / "new/create.txt") == "created", "Create missing");
    require(read(overlay.value().root / "modify.txt") == "new", "Modify missing");
    require(!fs::exists(overlay.value().root / "delete.txt"), "Delete missing");
    require(read(overlay.value().root / "renamed.txt") == "rename", "Rename missing");
    require(!fs::exists(overlay.value().root / ".git"), ".git was copied");
    require(read(fixture.workspace.path / "modify.txt") == "old"
            && fs::exists(fixture.workspace.path / "delete.txt")
            && fs::exists(fixture.workspace.path / "rename-source.txt")
            && !fs::exists(fixture.workspace.path / "new/create.txt"),
        "Overlay mutated the real workspace");
    const auto fingerprint = overlay.value().overlayFingerprint;
    require(ManifestProposalOverlay::cleanup(overlay.value()).success(), "Overlay cleanup failed");

    const auto restarted = ManifestProposalOverlay::build(
        overlayRequest(fixture, {"p1"}), fixture.service);
    require(restarted.success() && restarted.value().overlayFingerprint == fingerprint,
        "Same baseline/proposal set did not reconstruct the same fingerprint");
    require(ManifestProposalOverlay::cleanup(restarted.value()).success(), "Restarted overlay cleanup failed");
}

void testOverlayExcludesUnselectedSiblingAndFailsMissingContent()
{
    Fixture fixture;
    fixture.add("selected", {change("a", FileChangeOperation::Create, "selected.txt", "yes")});
    fixture.add("sibling", {change("b", FileChangeOperation::Create, "sibling.txt", "no")});
    auto overlay = ManifestProposalOverlay::build(
        overlayRequest(fixture, {"selected"}), fixture.service);
    require(overlay.success() && !fs::exists(overlay.value().root / "sibling.txt"),
        "Unrelated sibling proposal leaked into overlay");
    require(ManifestProposalOverlay::cleanup(overlay.value()).success(), "Cleanup failed");

    fixture.add("missing", {change("missing-content", FileChangeOperation::Create, "missing.txt", "x")});
    fixture.store.proposed.erase({"missing", "missing-content"});
    const auto failed = ManifestProposalOverlay::build(
        overlayRequest(fixture, {"missing"}), fixture.service);
    require(!failed.success() && failed.error()->code == "overlay.proposed_content_unavailable",
        "Missing proposed content did not fail closed");
}

void testOverlayCapsAndCleanupContainmentFailClosed()
{
    Fixture fixture;
    write(fixture.workspace.path / "one.txt", "1");
    write(fixture.workspace.path / "two.txt", "2");
    auto request = overlayRequest(fixture, {});
    request.limits.maxFiles = 1;
    const auto capped = ManifestProposalOverlay::build(request, fixture.service);
    require(!capped.success() && capped.error()->code == "overlay.baseline_limit",
        "Overlay file-count cap did not fail before provider execution");

    ManifestProposalOverlayDescriptor forged;
    forged.root = fixture.workspace.path;
    forged.trustedScratchRoot = fixture.scratch.path;
    const auto denied = ManifestProposalOverlay::cleanup(forged);
    require(!denied.success() && fs::exists(fixture.workspace.path / "one.txt"),
        "Overlay cleanup containment guard allowed a broad/forged target");

    auto linked = overlayRequest(fixture, {});
    std::error_code error;
    fs::create_directory_symlink(fixture.workspace.path,
        fixture.workspace.path / "unsafe-link", error);
    if (!error) {
        const auto result = ManifestProposalOverlay::build(linked, fixture.service);
        require(!result.success() && result.error()->code == "overlay.reparse_point",
            "Overlay followed a symlink/reparse point");
    }
}

void testOverlayUsesPolicyVisibleSnapshotAndRejectsSourceDrift()
{
    Fixture fixture;
    write(fixture.workspace.path / "qml" / "Main.qml", "Item {}\n");
    write(fixture.workspace.path / "build" / "CMakeCache.txt",
        "CMAKE_HOME_DIRECTORY=C:/private/original/workspace\n");
    write(fixture.workspace.path / ".env", "SECRET_MUST_NOT_ENTER_OVERLAY");

    auto request = overlayRequest(fixture, {});
    const auto overlay = ManifestProposalOverlay::build(request, fixture.service);
    require(overlay.success(), "Source-only overlay did not build");
    require(fs::is_regular_file(overlay.value().root / "qml" / "Main.qml"),
        "Policy-visible source file was not copied");
    require(!fs::exists(overlay.value().root / "build")
            && !fs::exists(overlay.value().root / ".env"),
        "Denied generated or secret input entered the source-only overlay");
    require(overlay.value().baselineFingerprint
            == request.baselineSnapshot.fingerprint,
        "Overlay did not retain the trusted baseline identity");
    require(ManifestProposalOverlay::cleanup(overlay.value()).success(),
        "Source-only overlay cleanup failed");

    write(fixture.workspace.path / "qml" / "Main.qml", "Item { id: drift }\n");
    const auto drifted = ManifestProposalOverlay::build(request, fixture.service);
    require(!drifted.success()
            && drifted.error()->code == "overlay.baseline_mismatch",
        "Source drift after baseline capture did not fail closed");
}

// QML-P1 / QML-lint advisory-severity policy (2026-09-28): only the two
// evidence-backed advisory tags are ever non-Hard; every other tag,
// including ones this codebase has not seen before, fails closed as Hard.
void testClassifyQmlLintFindingSeverityRecognizesUnqualifiedAndUnusedImportsAsAdvisory()
{
    require(classifyQmlLintFindingSeverity("unqualified") == QmlLintFindingSeverity::Advisory,
        "unqualified must be a recognized Advisory tag");
    require(classifyQmlLintFindingSeverity("unused-imports") == QmlLintFindingSeverity::Advisory,
        "unused-imports must be a recognized Advisory tag (real Stage 4A gap, 2026-09-28)");
    for (const std::string_view tag : {"import", "syntax", "missing-property",
             "missing-type", "unresolved-type", "compiler", "", "some-future-category"}) {
        require(classifyQmlLintFindingSeverity(tag) == QmlLintFindingSeverity::Hard,
            "An unrecognized or known-semantic tag must fail closed as Hard");
    }
}

// Stage 7B Run 1c37cc37 (2026-10-07): qmllint files `no matching signal found for handler` under the
// SAME [unqualified] tag as the harmless "Unqualified access". At run time it is `Cannot assign to
// non-existent property` - the component does not load - yet the tag-only classifier made it
// Advisory, it passed every Backend check and the applied Dashboard stopped loading. The finding-level
// classification is Hard for that message whatever the tag, and unchanged for everything else.
void testClassifyQmlLintFindingMakesLoadBreakingMessagesHardUnderAnyTag()
{
    const std::string handler =
        "no matching signal found for handler \"onMaxItemsChanged\" [unqualified]";
    require(classifyQmlLintFindingSeverity("unqualified", handler) == QmlLintFindingSeverity::Hard,
        "A handler for a signal the object does not have must be Hard under the unqualified tag");
    require(classifyQmlLintFindingSeverity("unqualified",
                "no matching signal found for handler \"onFooChanged\"")
                == QmlLintFindingSeverity::Hard,
        "The message decides, with or without qmllint's own tag suffix in the text");
    require(classifyQmlLintFindingSeverity("unused-imports", handler) == QmlLintFindingSeverity::Hard
            && classifyQmlLintFindingSeverity("", handler) == QmlLintFindingSeverity::Hard,
        "The message makes the finding Hard whatever tag it was reported under");

    require(classifyQmlLintFindingSeverity("unqualified", "Unqualified access [unqualified]")
                == QmlLintFindingSeverity::Advisory
            && classifyQmlLintFindingSeverity("unqualified", "Unqualified access")
                == QmlLintFindingSeverity::Advisory
            && classifyQmlLintFindingSeverity("unqualified", "")
                == QmlLintFindingSeverity::Advisory
            && classifyQmlLintFindingSeverity("unused-imports", "Unused import [unused-imports]")
                == QmlLintFindingSeverity::Advisory,
        "The harmless findings the advisory policy exists for must stay Advisory");
    require(classifyQmlLintFindingSeverity("import", "x") == QmlLintFindingSeverity::Hard
            && classifyQmlLintFindingSeverity("missing-property",
                   "Could not find property \"notAProperty\".") == QmlLintFindingSeverity::Hard,
        "Findings under a semantic tag stay Hard");
    require(classifyQmlLintFindingSeverity("unqualified") == QmlLintFindingSeverity::Advisory,
        "The tag-level default is unchanged");
}

ManifestVerificationDiagnosticFinding makeFinding(
    std::string fileKey, int line, int column, std::string tag, std::string text)
{
    ManifestVerificationDiagnosticFinding finding;
    finding.fileKey = std::move(fileKey);
    finding.line = line;
    finding.column = column;
    finding.tag = std::move(tag);
    finding.text = std::move(text);
    return finding;
}

// QML-P2: a finding's identity must never depend on line/column - only an
// unrelated earlier edit shifting every later line would otherwise be
// miscounted as churn.
void testFingerprintQmlLintFindingIgnoresLineAndColumn()
{
    const auto a = makeFinding("qml/App.qml", 4, 1, "unqualified", "Unqualified access");
    const auto b = makeFinding("qml/App.qml", 9, 3, "unqualified", "Unqualified access");
    require(fingerprintQmlLintFinding(a) == fingerprintQmlLintFinding(b),
        "Line/column must not affect a finding's fingerprint");
    const auto differentFile = makeFinding("qml/Other.qml", 4, 1, "unqualified", "Unqualified access");
    const auto differentTag = makeFinding("qml/App.qml", 4, 1, "deprecated", "Unqualified access");
    const auto differentText = makeFinding("qml/App.qml", 4, 1, "unqualified", "Different message");
    require(fingerprintQmlLintFinding(a) != fingerprintQmlLintFinding(differentFile)
            && fingerprintQmlLintFinding(a) != fingerprintQmlLintFinding(differentTag)
            && fingerprintQmlLintFinding(a) != fingerprintQmlLintFinding(differentText),
        "fileKey/tag/text must each affect a finding's fingerprint");
}

// QML-P2 hardening: a before/after pair reconstructed from two independent
// candidate overlays (e.g. a source Run's original candidate vs a repair
// child's successor) is not guaranteed to spell the same real file's
// fileKey identically - one side may carry the "<candidate>/" wrapper, use
// backslashes, or differ in case. The fingerprint must still match.
void testFingerprintQmlLintFindingCanonicalizesFileKeyAcrossCandidateWrapperCaseAndSeparators()
{
    const auto wrapped = makeFinding(
        "<candidate>/qml/AppSidebar.qml", 4, 1, "unqualified", "Unqualified access");
    const auto bare = makeFinding(
        "qml/AppSidebar.qml", 9, 3, "unqualified", "Unqualified access");
    const auto backslashed = makeFinding(
        "<candidate>\\qml\\AppSidebar.qml", 1, 1, "unqualified", "Unqualified access");
    const auto upperCased = makeFinding(
        "<candidate>/QML/AppSidebar.QML", 1, 1, "unqualified", "Unqualified access");
    require(fingerprintQmlLintFinding(wrapped) == fingerprintQmlLintFinding(bare)
            && fingerprintQmlLintFinding(wrapped) == fingerprintQmlLintFinding(backslashed)
            && fingerprintQmlLintFinding(wrapped) == fingerprintQmlLintFinding(upperCased),
        "The same real file must fingerprint identically regardless of candidate wrapper, "
        "separator style, or case - two sides of a before/after comparison must never diverge "
        "on cosmetic path spelling alone");
}

void testComputeQmlLintDiagnosticDeltaClassifiesEliminatedNewAndRetainedAcrossLineMovement()
{
    const std::vector<ManifestVerificationDiagnosticFinding> before = {
        makeFinding("qml/AppSidebar.qml", 12, 4, "unqualified", "Unqualified access"),
        makeFinding("qml/Legacy.qml", 3, 1, "deprecated", "Deprecated API used")};
    // The retained finding's line shifted (12 -> 20, e.g. because an earlier
    // unrelated edit added lines above it); the deprecated one was fixed;
    // a brand-new import failure appeared in a different file.
    const std::vector<ManifestVerificationDiagnosticFinding> after = {
        makeFinding("qml/AppSidebar.qml", 20, 4, "unqualified", "Unqualified access"),
        makeFinding("qml/PageHost.qml", 1, 1, "import", "Failed to import module")};

    const auto delta = computeQmlLintDiagnosticDelta(before, after);
    require(delta.eliminatedCount == 1 && delta.newCount == 1 && delta.retainedCount == 1,
        "Delta must classify exactly one eliminated, one new, one retained finding");
    require(delta.retainedHardCount == 0 && delta.retainedAdvisoryCount == 1,
        "The retained finding is the Advisory unqualified one, not the new Hard import failure");
    const auto retained = std::find_if(delta.entries.begin(), delta.entries.end(),
        [](const QmlLintFindingDeltaEntry& entry) {
            return entry.kind == QmlLintFindingDeltaKind::Retained;
        });
    require(retained != delta.entries.end() && retained->finding.line == 20,
        "A retained entry must reflect the AFTER finding's real location, not a stale one");
}

// QML-P2: two findings sharing one generic (fileKey, tag, text) fingerprint
// - the realistic shape when a file has two distinct unqualified references
// with an identical generic message - must still be compared correctly by
// count, not silently collapsed into a single logical finding.
void testComputeQmlLintDiagnosticDeltaUsesMultisetSemanticsForDuplicateFingerprints()
{
    const std::vector<ManifestVerificationDiagnosticFinding> before = {
        makeFinding("qml/AppSidebar.qml", 5, 1, "unqualified", "Unqualified access"),
        makeFinding("qml/AppSidebar.qml", 12, 1, "unqualified", "Unqualified access")};
    const std::vector<ManifestVerificationDiagnosticFinding> after = {
        makeFinding("qml/AppSidebar.qml", 5, 1, "unqualified", "Unqualified access")};

    const auto delta = computeQmlLintDiagnosticDelta(before, after);
    require(delta.eliminatedCount == 1 && delta.retainedCount == 1 && delta.newCount == 0,
        "One real fix among two identically-worded findings must count as one eliminated, "
        "one retained - never as zero progress or a full match");
}

ManifestVerificationCheckResult makeCheckResult(
    std::string profileId, ExecutionVerificationKind kind, ExecutionVerificationStatus status)
{
    ManifestVerificationCheckResult check;
    check.profileId = std::move(profileId);
    check.kind = kind;
    check.status = status;
    return check;
}

ManifestVerificationDiagnostic makeDiagnostic(std::string code, std::string requirementId,
    std::vector<ManifestVerificationDiagnosticFinding> findings)
{
    ManifestVerificationDiagnostic diagnostic;
    diagnostic.code = std::move(code);
    diagnostic.requirementId = std::move(requirementId);
    diagnostic.structuredFindings = std::move(findings);
    return diagnostic;
}

// QML-P0: proposal safety and CMakeBuild both passed; the only failure is
// QmlLint, and its only structured finding is the one recognized Advisory
// tag - the real shape of the 2026-09-17 paid canary's remaining findings.
void testClassifyManifestContentOutcomeRecognizesBoundedAdvisoryLintFailureAsNeedsManualAction()
{
    ManifestProposalSetVerification verification;
    verification.proposalSafetyStatus = ExecutionVerificationStatus::Passed;
    verification.checks = {
        makeCheckResult("build", ExecutionVerificationKind::CMakeBuild,
            ExecutionVerificationStatus::Passed),
        makeCheckResult("lint", ExecutionVerificationKind::QmlLint,
            ExecutionVerificationStatus::Failed)};
    verification.diagnostics = {makeDiagnostic("verification.qml_warning", "lint",
        {makeFinding("qml/AppSidebar.qml", 4, 1, "unqualified", "Unqualified access")})};
    require(classifyManifestContentOutcome(verification)
            == ManifestContentOutcome::NeedsManualAction,
        "Proposal-safety-clean, build-clean, Advisory-only lint failure must classify as "
        "NeedsManualAction");
}

// A broken-component finding is not "bounded advisory evidence a human can look at": it is a Hard
// failure, so the content outcome stays undifferentiated (normal repair path), never manual action.
void testClassifyManifestContentOutcomeTreatsMissingSignalHandlerAsHardNotManualAction()
{
    ManifestProposalSetVerification verification;
    verification.proposalSafetyStatus = ExecutionVerificationStatus::Passed;
    verification.checks = {
        makeCheckResult("build", ExecutionVerificationKind::CMakeBuild,
            ExecutionVerificationStatus::Passed),
        makeCheckResult("lint", ExecutionVerificationKind::QmlLint,
            ExecutionVerificationStatus::Failed)};
    verification.diagnostics = {makeDiagnostic("verification.qml_warning", "lint",
        {makeFinding("qml/components/dashboard/RecentAlertsPanel.qml", 110, 9, "unqualified",
            "no matching signal found for handler \"onMaxItemsChanged\" [unqualified]")})};
    require(classifyManifestContentOutcome(verification) == ManifestContentOutcome::Undifferentiated,
        "A finding that breaks the component must never classify as bounded manual action");
}

void testClassifyManifestContentOutcomeFailsClosedWhenProposalSafetyFailed()
{
    ManifestProposalSetVerification verification;
    verification.proposalSafetyStatus = ExecutionVerificationStatus::Failed;
    verification.checks = {makeCheckResult("lint", ExecutionVerificationKind::QmlLint,
        ExecutionVerificationStatus::Failed)};
    verification.diagnostics = {makeDiagnostic("verification.qml_warning", "lint",
        {makeFinding("qml/AppSidebar.qml", 4, 1, "unqualified", "Unqualified access")})};
    require(classifyManifestContentOutcome(verification) == ManifestContentOutcome::Undifferentiated,
        "Failed proposal safety must never classify as NeedsManualAction regardless of lint");
}

void testClassifyManifestContentOutcomeFailsClosedWhenANonLintCheckFailed()
{
    ManifestProposalSetVerification verification;
    verification.proposalSafetyStatus = ExecutionVerificationStatus::Passed;
    verification.checks = {
        makeCheckResult("build", ExecutionVerificationKind::CMakeBuild,
            ExecutionVerificationStatus::Failed),
        makeCheckResult("lint", ExecutionVerificationKind::QmlLint,
            ExecutionVerificationStatus::Failed)};
    verification.diagnostics = {makeDiagnostic("verification.qml_warning", "lint",
        {makeFinding("qml/AppSidebar.qml", 4, 1, "unqualified", "Unqualified access")})};
    require(classifyManifestContentOutcome(verification) == ManifestContentOutcome::Undifferentiated,
        "A failed CMakeBuild check must never be masked by an otherwise-bounded lint failure");
}

void testClassifyManifestContentOutcomeFailsClosedOnAnyHardLintFinding()
{
    ManifestProposalSetVerification verification;
    verification.proposalSafetyStatus = ExecutionVerificationStatus::Passed;
    verification.checks = {makeCheckResult("lint", ExecutionVerificationKind::QmlLint,
        ExecutionVerificationStatus::Failed)};
    verification.diagnostics = {makeDiagnostic("verification.qml_warning", "lint",
        {makeFinding("qml/AppSidebar.qml", 4, 1, "unqualified", "Unqualified access"),
         makeFinding("qml/PageHost.qml", 1, 1, "import", "Failed to import module")})};
    require(classifyManifestContentOutcome(verification) == ManifestContentOutcome::Undifferentiated,
        "A single Hard finding among otherwise-Advisory findings must still fail closed");
}

void testClassifyManifestContentOutcomeIgnoresHardBaselineDebtBesideRepairableWarning()
{
    ManifestProposalSetVerification verification;
    verification.proposalSafetyStatus = ExecutionVerificationStatus::Passed;
    verification.checks = {makeCheckResult("lint", ExecutionVerificationKind::QmlLint,
        ExecutionVerificationStatus::Failed)};
    verification.diagnostics = {
        makeDiagnostic("verification.qml_warning_baseline_debt", "lint",
            {makeFinding("qml/Legacy.qml", 1, 1, "import", "Failed to import module")}),
        makeDiagnostic("verification.qml_warning", "lint",
            {makeFinding("qml/AppSidebar.qml", 4, 1, "unqualified", "Unqualified access")})};
    require(classifyManifestContentOutcome(verification)
            == ManifestContentOutcome::NeedsManualAction,
        "Proven baseline debt must not hide a separate repairable candidate warning");
    require(isIgnorableNonRepairDiagnosticCode("verification.qml_warning_baseline_debt"),
        "Baseline-debt evidence must remain ignorable for continuation admission");
}

// Regression for Runs 030ce232/9a344ee3: the baseline-aware lint check emits the
// harmless baseline-debt diagnostic first, so diagnostics.front() named it as
// the failure reason while the real blocker (outside_manifest) was hidden.
void testPrimaryFailureDiagnosticSkipsNonBlockingDebtAndAdvisory()
{
    ManifestProposalSetVerification verification;
    require(primaryFailureDiagnostic(verification) == nullptr,
        "No diagnostics must yield no primary failure diagnostic");

    verification.diagnostics = {
        makeDiagnostic("verification.qml_warning_baseline_debt", "lint",
            {makeFinding("qml/Legacy.qml", 1, 1, "unqualified", "Unqualified access")}),
        makeDiagnostic("verification.qml_warning_advisory", "lint",
            {makeFinding("qml/A.qml", 2, 1, "unqualified", "Unqualified access")}),
        makeDiagnostic("verification.qml_warning_outside_manifest", "lint",
            {makeFinding("qml/B.qml", 3, 1, "unqualified", "Unqualified access")})};
    const auto* primary = primaryFailureDiagnostic(verification);
    require(primary != nullptr && primary->code == "verification.qml_warning_outside_manifest",
        "Baseline debt and advisories must never be named as the reason verification failed");

    verification.diagnostics = {
        makeDiagnostic("verification.qml_warning_baseline_debt", "lint",
            {makeFinding("qml/Legacy.qml", 1, 1, "unqualified", "Unqualified access")}),
        makeDiagnostic("verification.qml_warning_advisory", "lint",
            {makeFinding("qml/A.qml", 2, 1, "unqualified", "Unqualified access")})};
    primary = primaryFailureDiagnostic(verification);
    require(primary != nullptr && primary->code == "verification.qml_warning_baseline_debt",
        "When every diagnostic is non-blocking the first one is the fallback");
}

// Stage 8B Run 969e35fa...: the Run failed on `no matching signal found for handler "onAction"` but its
// terminal message, report and advice input only said "QML lint emitted one or more warnings".
const char* const kHandlerFindingText = "no matching signal found for handler \"onAction\" [unqualified]";

ManifestVerificationDiagnostic makeLintDiagnostic(std::string code, std::string message,
    std::vector<ManifestVerificationDiagnosticFinding> findings)
{
    auto diagnostic = makeDiagnostic(std::move(code), "lint", std::move(findings));
    diagnostic.message = std::move(message);
    return diagnostic;
}

void testRenderQmlLintFindingLabelUsesDisplayPathAndNeverLeaksAnAbsolutePath()
{
    require(renderQmlLintFindingLabel(makeFinding("<candidate>/qml/views/ProductionOverviewView.qml",
                192, 17, "unqualified", kHandlerFindingText))
            == std::string("qml/views/ProductionOverviewView.qml:192:17 ") + kHandlerFindingText,
        "A finding label must read path:line:column text with the candidate-relative path");
    const auto leaked = renderQmlLintFindingLabel(
        makeFinding("C:/Users/someone/secret/qml/X.qml", 3, 1, "import", "Failed to import"));
    require(leaked.rfind("X.qml:3:1 ", 0) == 0 && leaked.find("secret") == std::string::npos
            && leaked.find("someone") == std::string::npos,
        "A key without a safe candidate prefix must be reduced to its file name, never a full path");
    require(renderQmlLintFindingLabel(makeFinding("<candidate>/qml/A.qml", 0, 0, "x", "Odd"))
            == "qml/A.qml Odd",
        "A finding without a location must not print :0:0");
    const auto multiline = renderQmlLintFindingLabel(
        makeFinding("<candidate>/qml/A.qml", 1, 2, "x", "first\nsecond\r\nthird"));
    require(multiline.find('\n') == std::string::npos && multiline.find('\r') == std::string::npos,
        "A label is one line");
}

void testDescribeDiagnosticFindingsIsBoundedAndSkipsNonBlockingAndRepeatedMessages()
{
    std::vector<ManifestVerificationDiagnosticFinding> five;
    for (int index = 1; index <= 5; ++index) {
        five.push_back(makeFinding("<candidate>/qml/F" + std::to_string(index) + ".qml", index, 1,
            "import", "Problem " + std::to_string(index)));
    }
    const auto blocking = makeLintDiagnostic(
        "verification.qml_warning", "QML lint emitted one or more warnings", five);
    const auto described = describeDiagnosticFindings(blocking, 3);
    require(described == "qml/F1.qml:1:1 Problem 1; qml/F2.qml:2:1 Problem 2; "
                         "qml/F3.qml:3:1 Problem 3 (+2 more)",
        "The terminal excerpt must name the first findings, bounded, with the omitted count");
    require(describeDiagnosticFindings(blocking, 5).find("more") == std::string::npos,
        "Nothing is omitted when every finding fits");
    require(describeDiagnosticFindings(blocking, 0).empty(), "A zero cap names nothing");
    require(describeDiagnosticFindings(
                makeLintDiagnostic("verification.qml_warning_baseline_debt", "debt", five), 3).empty()
            && describeDiagnosticFindings(
                makeLintDiagnostic("verification.qml_warning_advisory", "advisory", five), 3).empty(),
        "Baseline debt and advisories are never named as a failure reason");
    require(describeDiagnosticFindings(
                makeLintDiagnostic("verification.qml_warning", "msg", {}), 3).empty(),
        "A diagnostic without structured findings adds nothing");
    require(describeDiagnosticFindings(makeLintDiagnostic("verification.qml_warning",
                std::string("excerpt: ") + kHandlerFindingText,
                {makeFinding("<candidate>/qml/V.qml", 192, 17, "unqualified", kHandlerFindingText)}),
                3).empty(),
        "A message that already carries the first finding's text is not repeated");
}

void testRenderVerificationFailureReasonAppendsTheBlockingFindings()
{
    const auto handler = makeFinding("<candidate>/qml/views/ProductionOverviewView.qml", 192, 17,
        "unqualified", kHandlerFindingText);
    require(renderVerificationFailureReason(makeLintDiagnostic("verification.qml_warning",
                "QML lint emitted one or more warnings", {handler}), 3)
            == std::string("[verification.qml_warning] QML lint emitted one or more warnings: "
                           "qml/views/ProductionOverviewView.qml:192:17 ") + kHandlerFindingText,
        "The Run's terminal reason must name the blocking finding, not only the generic message");
    require(renderVerificationFailureReason(
                makeLintDiagnostic("verification.required_output_missing", "Required output missing",
                    {}), 3)
            == "[verification.required_output_missing] Required output missing",
        "A diagnostic without findings keeps its historical reason text");
}

void testRenderVerificationDiagnosticLinesNamesBlockingFindingsAndOnlyCountsNonBlocking()
{
    std::vector<ManifestVerificationDiagnosticFinding> ten;
    for (int index = 1; index <= 10; ++index) {
        ten.push_back(makeFinding("<candidate>/qml/G" + std::to_string(index) + ".qml", index, 1,
            "import", "Problem " + std::to_string(index)));
    }
    const std::vector<ManifestVerificationDiagnostic> diagnostics = {
        makeLintDiagnostic("verification.qml_warning_baseline_debt",
            "QML lint warnings are confined to unchanged pre-existing baseline debt",
            {makeFinding("<candidate>/qml/Old.qml", 1, 1, "unqualified", "Unqualified access"),
             makeFinding("<candidate>/qml/Old.qml", 2, 1, "unqualified", "Unqualified access")}),
        makeLintDiagnostic("verification.qml_warning", "QML lint emitted one or more warnings",
            {makeFinding("<candidate>/qml/views/ProductionOverviewView.qml", 192, 17, "unqualified",
                kHandlerFindingText)}),
        makeLintDiagnostic("verification.qml_warning_advisory", "advisory",
            {makeFinding("<candidate>/qml/Adv.qml", 5, 1, "unqualified", "Unqualified access")}),
        makeLintDiagnostic("verification.required_output_missing", "Required output missing", {})};
    const auto text = renderVerificationDiagnosticLines(diagnostics, 8);
    require(text
            == std::string(
                   "- [verification.qml_warning_baseline_debt] QML lint warnings are confined to "
                   "unchanged pre-existing baseline debt (lint)\n"
                   "  - 2 finding(s), not blocking\n"
                   "- [verification.qml_warning] QML lint emitted one or more warnings (lint)\n"
                   "  - qml/views/ProductionOverviewView.qml:192:17 ") + kHandlerFindingText + "\n"
                   "- [verification.qml_warning_advisory] advisory (lint)\n"
                   "  - 1 finding(s), not blocking\n"
                   "- [verification.required_output_missing] Required output missing (lint)\n",
        "The report must name the blocking finding, only count the non-blocking ones and render a "
        "diagnostic without findings exactly as before");

    const auto capped = renderVerificationDiagnosticLines(
        {makeLintDiagnostic("verification.qml_warning", "msg", ten)}, 8);
    std::size_t lines = 0;
    for (const char ch : capped) lines += ch == '\n' ? 1 : 0;
    require(lines == 1 + 8 + 1 && capped.find("qml/G8.qml:8:1 Problem 8") != std::string::npos
            && capped.find("G9.qml") == std::string::npos
            && capped.find("  - (+2 more)\n") != std::string::npos,
        "A blocking diagnostic lists at most the per-diagnostic cap and counts the rest");
    require(renderVerificationDiagnosticLines({}, 8).empty(), "No diagnostics render nothing");

    // A diagnostic whose flat message already IS the excerpt is not repeated as nested lines.
    require(renderVerificationDiagnosticLines(
                {makeLintDiagnostic("verification.command_failed",
                    std::string("excerpt: ") + kHandlerFindingText,
                    {makeFinding("<candidate>/qml/V.qml", 192, 17, "unqualified", kHandlerFindingText)})},
                8)
            == std::string("- [verification.command_failed] excerpt: ") + kHandlerFindingText + " (lint)\n",
        "The report must not repeat findings a diagnostic's own message already carries");
}

void testClassifyManifestContentOutcomeFailsClosedWhenFailedLintHasNoStructuredEvidence()
{
    ManifestProposalSetVerification verification;
    verification.proposalSafetyStatus = ExecutionVerificationStatus::Passed;
    verification.checks = {makeCheckResult("lint", ExecutionVerificationKind::QmlLint,
        ExecutionVerificationStatus::NotVerifiable)};
    verification.diagnostics = {makeDiagnostic(
        "verification.invalid_profile_arguments", "lint", {})};
    require(classifyManifestContentOutcome(verification) == ManifestContentOutcome::Undifferentiated,
        "A failed QmlLint check with no structured findings (an infra-level failure, not real "
        "qmllint output) must fail closed, never be treated as bounded");
}

// Even a QmlLint check sitting in NotVerifiable/NotRun (an infra-level
// uncertainty, never a real, understood lint outcome) that happens to carry
// Advisory-only structured findings from some other source must still fail
// closed - only a genuinely Failed QmlLint check is bounded/lint-only
// evidence.
void testClassifyManifestContentOutcomeFailsClosedOnNotVerifiableQmlLintEvenWithAdvisoryFindings()
{
    ManifestProposalSetVerification verification;
    verification.proposalSafetyStatus = ExecutionVerificationStatus::Passed;
    verification.checks = {makeCheckResult("lint", ExecutionVerificationKind::QmlLint,
        ExecutionVerificationStatus::NotVerifiable)};
    verification.diagnostics = {makeDiagnostic("verification.qml_warning", "lint",
        {makeFinding("qml/AppSidebar.qml", 4, 1, "unqualified", "Unqualified access")})};
    require(classifyManifestContentOutcome(verification) == ManifestContentOutcome::Undifferentiated,
        "A NotVerifiable QmlLint check must fail closed even with only Advisory findings present - "
        "only a genuinely Failed check is real, bounded lint evidence");
}

void testClassifyManifestContentOutcomeIsUndifferentiatedWhenNothingFailed()
{
    ManifestProposalSetVerification verification;
    verification.proposalSafetyStatus = ExecutionVerificationStatus::Passed;
    verification.checks = {
        makeCheckResult("build", ExecutionVerificationKind::CMakeBuild,
            ExecutionVerificationStatus::Passed),
        makeCheckResult("lint", ExecutionVerificationKind::QmlLint,
            ExecutionVerificationStatus::Passed)};
    require(classifyManifestContentOutcome(verification) == ManifestContentOutcome::Undifferentiated,
        "A fully passing verification is not this policy's concern - the terminal status "
        "already tells the truth");
}

void testCombinedConflictAndWarningBearingQmlLintFail()
{
    Fixture fixture;
    write(fixture.workspace.path / "shared.qml", "old");
    fixture.add("p1", {change("a", FileChangeOperation::Modify, "shared.qml", "one")});
    fixture.add("p2", {change("b", FileChangeOperation::Modify, "shared.qml", "two")});
    auto overlay = ManifestProposalOverlay::build(
        overlayRequest(fixture, {"p1", "p2"}), fixture.service);
    require(overlay.success(), "Conflict fixture overlay must materialize for combined audit");

    ExecutionAcceptanceRequirement requirement;
    requirement.id = "qml-lint";
    requirement.description = "QML lint is warning-free";
    requirement.verificationKind = ExecutionVerificationKind::QmlLint;
    requirement.verificationArguments = {{"buildDirectory", "build"}, {"target", "all_qmllint"}};
    FakeCommandRunner runner;
    ManifestVerificationCommandOutcome command;
    command.started = true;
    command.exitCode = 0;
    command.boundedOutput = "AppSidebar.qml:33: warning: unqualified access [unqualified]\n"
        "AppSidebar.qml:34: warning: unqualified access [unqualified]\n"
        "AppSidebar.qml:39: warning: unqualified access [unqualified]\n";
    runner.outcomes.push_back(command);
    ManifestProposalSetVerifierRequest request;
    request.overlay = overlay.value();
    request.tasks = {taskWithRequirement(std::move(requirement))};
    const auto verification = ManifestProposalSetVerifier::verify(
        request, fixture.service, runner, {});
    require(verification.success(), "Verifier did not return typed failure result");
    require(verification.value().proposalSafetyStatus == ExecutionVerificationStatus::Failed,
        "Cross-proposal conflict was not detected");
    require(verification.value().buildLintTestStatus == ExecutionVerificationStatus::Failed
            && verification.value().acceptanceStatus == ExecutionVerificationStatus::Failed
            && !requiredAcceptancePassed(verification.value()),
        "Warning-bearing QML lint incorrectly passed");
    require(ManifestProposalOverlay::cleanup(overlay.value()).success(), "Cleanup failed");
}

void testCorrectedFixturePassesAndMissingToolIsNotVerifiable()
{
    Fixture fixture;
    fixture.add("p1", {change("a", FileChangeOperation::Create, "AppSidebar.qml", "clean")});
    auto overlay = ManifestProposalOverlay::build(
        overlayRequest(fixture, {"p1"}), fixture.service);
    require(overlay.success(), "Clean fixture overlay failed");

    ExecutionAcceptanceRequirement requirement;
    requirement.id = "qml-lint";
    requirement.description = "QML lint is warning-free";
    requirement.requiredOutputs = {"AppSidebar.qml"};
    requirement.verificationKind = ExecutionVerificationKind::QmlLint;
    requirement.verificationArguments = {{"buildDirectory", "build"}, {"target", "all_qmllint"}};
    auto task = taskWithRequirement(requirement);
    ManifestProposalSetVerifierRequest request {overlay.value(), {task}};
    FakeCommandRunner passing;
    ManifestVerificationCommandOutcome pass;
    pass.started = true;
    pass.exitCode = 0;
    pass.boundedOutput = "Built target all_qmllint";
    passing.outcomes.push_back(pass);
    const auto verified = ManifestProposalSetVerifier::verify(
        request, fixture.service, passing, {});
    require(verified.success() && requiredAcceptancePassed(verified.value()),
        "Clean deterministic fixture did not pass");

    FakeCommandRunner missing;
    ManifestVerificationCommandOutcome unavailable;
    unavailable.errorCode = "verification.process_start_failed";
    missing.outcomes.push_back(unavailable);
    const auto unknown = ManifestProposalSetVerifier::verify(
        request, fixture.service, missing, {});
    require(unknown.success()
            && unknown.value().acceptanceStatus == ExecutionVerificationStatus::NotVerifiable
            && !requiredAcceptancePassed(unknown.value()),
        "Missing verification tool did not fail closed as not_verifiable");

    for (const auto& closedOutcome : std::vector<ManifestVerificationCommandOutcome> {
             [] { ManifestVerificationCommandOutcome value; value.started = true;
                  value.timedOut = true; return value; }(),
             [] { ManifestVerificationCommandOutcome value; value.started = true;
                  value.outputTruncated = true; value.exitCode = 0; return value; }(),
             [] { ManifestVerificationCommandOutcome value; value.started = true;
                  value.cancelled = true; return value; }()}) {
        FakeCommandRunner closedRunner;
        closedRunner.outcomes.push_back(closedOutcome);
        const auto closed = ManifestProposalSetVerifier::verify(
            request, fixture.service, closedRunner, {});
        require(closed.success()
                && closed.value().acceptanceStatus
                    == ExecutionVerificationStatus::Failed
                && !requiredAcceptancePassed(closed.value()),
            "Timeout/output-overflow/cancellation did not fail closed");
    }
    require(ManifestProposalOverlay::cleanup(overlay.value()).success(), "Cleanup failed");
}

void testFailedFreshConfigureSkipsLintAndSanitizesDiagnostic()
{
    Fixture fixture;
    const auto overlay = ManifestProposalOverlay::build(
        overlayRequest(fixture, {}), fixture.service);
    require(overlay.success(), "Configure-failure fixture overlay failed");

    ExecutionAcceptanceRequirement lint;
    lint.id = "lint";
    lint.description = "QML lint succeeds";
    lint.verificationKind = ExecutionVerificationKind::QmlLint;
    lint.verificationArguments = {
        {"buildDirectory", "build"}, {"target", "all_qmllint"}};
    ExecutionAcceptanceRequirement build;
    build.id = "build";
    build.description = "Clean configure and build succeed";
    build.verificationKind = ExecutionVerificationKind::CMakeBuild;
    build.verificationArguments = {
        {"preset", "local-qt-debug"},
        {"buildPreset", "local-qt-debug"}};

    ExecutionAcceptanceContract contract;
    // Deliberately place lint first: Backend prerequisite scheduling must not
    // depend on provider-authored requirement order.
    contract.requirements = {lint, build};
    ExecutionManifestTask task;
    task.id = "verify";
    task.objective = "Verify the clean candidate";
    task.role = "verifier";
    task.requestedCapability = WorkspaceCapability::Propose;
    task.acceptanceContract = std::move(contract);

    FakeCommandRunner runner;
    ManifestVerificationCommandOutcome configureFailure;
    configureFailure.started = true;
    configureFailure.exitCode = 1;
    configureFailure.boundedOutput =
        "password=DO_NOT_PERSIST\nCMake Error at "
        + overlay.value().root.string() + "/CMakeLists.txt: candidate configure failed\n";
    runner.outcomes.push_back(std::move(configureFailure));
    const auto verification = ManifestProposalSetVerifier::verify(
        {overlay.value(), {task}}, fixture.service, runner, {});

    require(verification.success(),
        "Configure failure did not produce a typed verification result");
    require(runner.calls == 1
            && runner.requests.front().commandIdentity
                == "cmake.configure.preset",
        "Lint or build ran after clean configure failed");
    require(verification.value().checks.size() == 2
            && verification.value().checks[0].profileId == "lint"
            && verification.value().checks[0].status
                == ExecutionVerificationStatus::NotRun
            && verification.value().checks[1].profileId == "build"
            && verification.value().checks[1].status
                == ExecutionVerificationStatus::Failed,
        "Prerequisite failure was not represented in original criterion order");
    const auto& diagnostics = verification.value().diagnostics;
    const auto commandFailure = std::find_if(diagnostics.begin(), diagnostics.end(),
        [](const auto& diagnostic) {
            return diagnostic.code == "verification.command_failed";
        });
    const auto prerequisite = std::find_if(diagnostics.begin(), diagnostics.end(),
        [](const auto& diagnostic) {
            return diagnostic.code == "verification.prerequisite_failed";
        });
    require(commandFailure != diagnostics.end()
            && prerequisite != diagnostics.end(),
        "Configure/lint prerequisite diagnostics are incomplete");
    require(commandFailure->message.find("DO_NOT_PERSIST") == std::string::npos
            && commandFailure->message.find(overlay.value().root.string())
                == std::string::npos
            && commandFailure->message.find("<candidate>") != std::string::npos,
        "Bounded command diagnostic leaked a secret or absolute candidate path");
    require(ManifestProposalOverlay::cleanup(overlay.value()).success(),
        "Configure-failure overlay cleanup failed");
}

// Reproduces a real Run: a QmlLint requirement with missing/invalid
// verificationArguments (no buildDirectory/target) is built manually by the
// verifier - status NotVerifiable, diagnostics recorded, but its
// boundedOutputHash was never set (no command ever ran, so there is no real
// hash). Wrapping that empty hash in a one-element evidenceReferences entry
// produced a blank reference, which validateManifestProposalSetVerification
// correctly rejects as verification.invalid_evidence_reference - but that
// made ManifestProposalSetVerifier::verify() itself return a hard
// Result::failure instead of a completed verification reporting
// NotVerifiable, discarding every diagnostic collected so far (a real Run's
// manifest-verification.md showed only "not_run" everywhere, with no way to
// tell which requirement or task was actually the cause).
void testInvalidProfileArgumentsAreNotVerifiableNotHardFailure()
{
    Fixture fixture;
    fixture.add("p1", {change("a", FileChangeOperation::Create, "AppSidebar.qml", "clean")});
    auto overlay = ManifestProposalOverlay::build(
        overlayRequest(fixture, {"p1"}), fixture.service);
    require(overlay.success(), "Clean fixture overlay failed");

    ExecutionAcceptanceRequirement requirement;
    requirement.id = "qml-lint-bad-args";
    requirement.description = "QML lint is warning-free";
    requirement.verificationKind = ExecutionVerificationKind::QmlLint;
    // Deliberately missing buildDirectory/target.
    ManifestProposalSetVerifierRequest request {
        overlay.value(), {taskWithRequirement(requirement)}};
    FakeCommandRunner runner;
    const auto verification = ManifestProposalSetVerifier::verify(
        request, fixture.service, runner, {});

    require(verification.success(),
        "A NotVerifiable check must not hard-fail the whole verify() call");
    require(verification.value().acceptanceStatus == ExecutionVerificationStatus::NotVerifiable,
        "Missing profile arguments must report NotVerifiable");
    require(!requiredAcceptancePassed(verification.value()),
        "A NotVerifiable required criterion must never count as passed");
    require(runner.calls == 0,
        "No command may run when profile arguments are invalid");
    require(verification.value().criteria.size() == 1
            && verification.value().criteria.front().evidenceReferences.empty(),
        "A check with no real command output must report empty evidence, not a blank entry");
    const bool hasDiagnostic = std::any_of(
        verification.value().diagnostics.begin(), verification.value().diagnostics.end(),
        [](const ManifestVerificationDiagnostic& diagnostic) {
            return diagnostic.code == "verification.invalid_profile_arguments";
        });
    require(hasDiagnostic, "The invalid-arguments diagnostic must still be recorded");
    require(ManifestProposalOverlay::cleanup(overlay.value()).success(), "Cleanup failed");
}

void testVerificationPreflightRejectsMissingAndUnknownPresets()
{
    TempDir workspace;
    ExecutionAcceptanceRequirement requirement;
    requirement.id = "build";
    requirement.description = "Build succeeds";
    requirement.verificationKind = ExecutionVerificationKind::CMakeBuild;
    requirement.verificationArguments = {
        {"preset", "default"}, {"buildPreset", "default"}};

    FakeCommandRunner noPresetRunner;
    const auto noPreset = ManifestVerificationEnvironmentPreflight::validate(
        preflightRequest(workspace.path, requirement), noPresetRunner, {});
    require(!noPreset.success()
            && noPreset.error()->code == "verification.configure_preset_unavailable"
            && noPresetRunner.calls == 0,
        "Missing preset files did not fail before a command probe");

    write(workspace.path / "CMakePresets.json", R"({"version":6})");
    require(fs::is_regular_file(workspace.path / "CMakePresets.json"),
        "Preset fixture was not written");
    FakeCommandRunner unknownPresetRunner;
    unknownPresetRunner.outcomes.push_back(
        successfulProbe("Available configure presets:\n  \"other\""));
    const auto unknownPreset = ManifestVerificationEnvironmentPreflight::validate(
        preflightRequest(workspace.path, requirement), unknownPresetRunner, {});
    require(!unknownPreset.success(),
        "Unknown configure preset unexpectedly passed preflight");
    require(unknownPreset.error()->code
            == "verification.configure_preset_unavailable",
        "Unknown configure preset produced the wrong failure code");
    if (unknownPresetRunner.calls != 1) {
        throw std::runtime_error("Unknown configure preset used "
            + std::to_string(unknownPresetRunner.calls) + " command probes");
    }

    FakeCommandRunner unknownBuildPresetRunner;
    unknownBuildPresetRunner.outcomes.push_back(successfulProbe("\"default\""));
    unknownBuildPresetRunner.outcomes.push_back(successfulProbe("\"other\""));
    const auto unknownBuildPreset = ManifestVerificationEnvironmentPreflight::validate(
        preflightRequest(workspace.path, requirement), unknownBuildPresetRunner, {});
    require(!unknownBuildPreset.success()
            && unknownBuildPreset.error()->code
                == "verification.build_preset_unavailable"
            && unknownBuildPresetRunner.calls == 2,
        "Unknown CMake build preset was not rejected deterministically");
}

void testVerificationPreflightRequiresCleanConfigureForQmlLint()
{
    TempDir workspace;
    ExecutionAcceptanceRequirement requirement;
    requirement.id = "lint";
    requirement.description = "QML lint succeeds";
    requirement.verificationKind = ExecutionVerificationKind::QmlLint;
    requirement.verificationArguments = {
        {"buildDirectory", "build"}, {"target", "all_qmllint"}};

    // Even a configured baseline build is intentionally unusable: candidate
    // verification must configure its own clean source-only overlay.
    write(workspace.path / "build" / "CMakeCache.txt", "configured elsewhere");
    FakeCommandRunner runner;
    const auto result = ManifestVerificationEnvironmentPreflight::validate(
        preflightRequest(workspace.path, requirement), runner, {});
    require(!result.success()
            && result.error()->code
                == "verification.clean_configure_prerequisite_missing"
            && runner.calls == 0,
        "QML lint without a clean configure/build prerequisite was admitted");
}

void testVerificationPreflightAcceptsAvailableProfiles()
{
    TempDir workspace;
    write(workspace.path / "CMakePresets.json", R"({"version":6})");
    ExecutionManifestTask task;
    task.id = "verify";
    task.objective = "Verify candidate";
    task.role = "verifier";
    task.requestedCapability = WorkspaceCapability::Propose;
    ExecutionAcceptanceContract contract;
    ExecutionAcceptanceRequirement build;
    build.id = "build";
    build.description = "Build succeeds";
    build.verificationKind = ExecutionVerificationKind::CMakeBuild;
    build.verificationArguments = {
        {"preset", "default"}, {"buildPreset", "default"}};
    ExecutionAcceptanceRequirement lint;
    lint.id = "lint";
    lint.description = "QML lint succeeds";
    lint.verificationKind = ExecutionVerificationKind::QmlLint;
    lint.verificationArguments = {
        {"buildDirectory", "build"}, {"target", "all_qmllint"}};
    contract.requirements = {build, lint};
    task.acceptanceContract = std::move(contract);

    ManifestVerificationEnvironmentPreflightRequest request;
    request.workspaceRoot = workspace.path;
    request.tasks = {std::move(task)};
    FakeCommandRunner runner;
    runner.outcomes.push_back(successfulProbe("\"default\""));
    runner.outcomes.push_back(successfulProbe("\"default\""));
    const auto result = ManifestVerificationEnvironmentPreflight::validate(
        request, runner, {});
    require(result.success() && runner.calls == 2,
        "Available preset/build/target environment did not pass preflight");
}

void testSystemCommandRunnerDrainsShortProcessOutput()
{
#ifdef _WIN32
    SystemManifestVerificationCommandRunner runner;
    for (std::size_t iteration = 0; iteration < 16; ++iteration) {
        const auto marker = "short-process-output-" + std::to_string(iteration);
        ManifestVerificationCommandRequest request;
        request.commandIdentity = "cmake.echo.regression";
        request.executable = "cmake";
        request.arguments = {"-E", "echo", marker};
        request.workingDirectory = fs::current_path();
        request.timeout = std::chrono::seconds(15);
        request.maxOutputBytes = 4 * 1024;

        const auto outcome = runner.run(request, {});
        require(outcome.started && outcome.exitCode.has_value()
                && *outcome.exitCode == 0 && !outcome.outputTruncated,
            "Short CMake output probe did not complete successfully");
        require(outcome.boundedOutput.find(marker) != std::string::npos,
            "System command runner lost output published at process exit");
    }

    TempDir workspace;
    write(workspace.path / "CMakeUserPresets.json", R"({
  "version": 6,
  "configurePresets": [{
    "name": "local-qt-debug",
    "generator": "Ninja",
    "binaryDir": "${sourceDir}/build/local-qt-debug"
  }],
  "buildPresets": [{
    "name": "local-qt-debug",
    "configurePreset": "local-qt-debug"
  }]
})");
    ExecutionAcceptanceRequirement requirement;
    requirement.id = "build";
    requirement.description = "Configured build preset is available";
    requirement.verificationKind = ExecutionVerificationKind::CMakeBuild;
    requirement.verificationArguments = {
        {"preset", "local-qt-debug"},
        {"buildPreset", "local-qt-debug"}};
    const auto preflight = ManifestVerificationEnvironmentPreflight::validate(
        preflightRequest(workspace.path, requirement), runner, {});
    require(preflight.success(),
        "Standalone CMakeUserPresets configure/build profiles were not observed");
#endif
}

void testConcurrentOverlayReconstructionStress1000()
{
    Fixture fixture;
    fixture.add("p1", {change("a", FileChangeOperation::Create,
        "candidate.txt", "candidate")});
    std::string expectedFingerprint;
    for (std::size_t iteration = 0; iteration < 1'000; ++iteration) {
        auto buildOne = [&fixture]() {
            return ManifestProposalOverlay::build(
                overlayRequest(fixture, {"p1"}), fixture.service);
        };
        auto leftFuture = std::async(std::launch::async, buildOne);
        auto rightFuture = std::async(std::launch::async, buildOne);
        auto left = leftFuture.get();
        auto right = rightFuture.get();
        require(left.success() && right.success(),
            "Concurrent overlay reconstruction failed");
        if (expectedFingerprint.empty())
            expectedFingerprint = left.value().overlayFingerprint;
        require(left.value().overlayFingerprint == expectedFingerprint
                && right.value().overlayFingerprint == expectedFingerprint,
            "Concurrent overlay fingerprint was nondeterministic");
        require(ManifestProposalOverlay::cleanup(left.value()).success()
                && ManifestProposalOverlay::cleanup(right.value()).success(),
            "Concurrent overlay cleanup failed");
    }
    require(fs::is_empty(fixture.scratch.path),
        "Concurrent overlay stress left temporary candidates behind");
}

// ---------------------------------------------------------------------
// Manifest verification reliability hardening (P0): short overlay root,
// path-budget preflight, CMAKE_OBJECT_PATH_MAX detection, and meaningful
// leak-safe diagnostics.
// ---------------------------------------------------------------------

// #1: the new default scratch root ("aio-v" + a 16-hex-character leaf) is
// dramatically shorter than the old one ("ai-orchestrator-manifest-
// overlays" + a 36-character UUID leaf) - this is a pure string-length
// comparison, deliberately independent of any real filesystem I/O.
void testDefaultOverlayRootIsSignificantlyShorterThanOld()
{
    const std::string oldShape = std::string("ai-orchestrator-manifest-overlays")
        + "/" + "manifest-overlay-" + "aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee";
    const std::string newShape = std::string("aio-v") + "/" + "m-" + "0123456789abcdef";
    require(newShape.size() < oldShape.size() / 2,
        "The new default overlay root shape is not significantly shorter than the old one");
}

// #2: two overlays built against the same scratch root get distinct
// candidate directories.
void testCandidateDirectoriesAreUnique()
{
    Fixture fixture;
    fixture.add("p1", {change("a", FileChangeOperation::Create, "one.txt", "one")});
    const auto first = ManifestProposalOverlay::build(
        overlayRequest(fixture, {"p1"}), fixture.service);
    const auto second = ManifestProposalOverlay::build(
        overlayRequest(fixture, {"p1"}), fixture.service);
    require(first.success() && second.success(), "Both overlay builds must succeed");
    require(first.value().root != second.value().root,
        "Two overlay builds against the same scratch root produced the same candidate directory");
    require(ManifestProposalOverlay::cleanup(first.value()).success()
            && ManifestProposalOverlay::cleanup(second.value()).success(),
        "Cleanup failed for one of the two unique overlays");
}

// #3: a scratch root that overlaps the real workspace (either direction)
// must never be accepted.
void testScratchRootDisjointFromWorkspaceIsEnforced()
{
    Fixture fixture;
    auto overlappingBelow = overlayRequest(fixture, {});
    overlappingBelow.trustedScratchRoot = fixture.workspace.path / "scratch-inside-workspace";
    const auto below = ManifestProposalOverlay::build(overlappingBelow, fixture.service);
    require(!below.success() && below.error()->code == "overlay.scratch_overlap",
        "A scratch root nested inside the workspace was accepted");

    TempDir outer;
    auto overlappingAbove = overlayRequest(fixture, {});
    overlappingAbove.workspaceRoot = outer.path / "workspace-inside-scratch";
    fs::create_directories(overlappingAbove.workspaceRoot);
    overlappingAbove.trustedScratchRoot = outer.path;
    overlappingAbove.baselinePolicy = WorkspaceReadPolicy::secureDefaults(
        fs::absolute(overlappingAbove.workspaceRoot));
    const auto baseline = WorkspaceBaselineProvenance::capture(overlappingAbove.baselinePolicy);
    require(baseline.success(), "Nested-workspace baseline capture failed");
    overlappingAbove.baselineSnapshot = baseline.value();
    const auto above = ManifestProposalOverlay::build(overlappingAbove, fixture.service);
    require(!above.success() && above.error()->code == "overlay.scratch_overlap",
        "A workspace nested inside the scratch root was accepted");
}

// #4: a caller-provided scratch root (the ordinary Fixture case every other
// test in this file already exercises) is honored verbatim as the
// descriptor's trustedScratchRoot.
void testCallerProvidedScratchRootIsHonored()
{
    Fixture fixture;
    const auto overlay = ManifestProposalOverlay::build(
        overlayRequest(fixture, {}), fixture.service);
    require(overlay.success(), "Caller-provided scratch root overlay failed to build");
    std::error_code error;
    require(overlay.value().trustedScratchRoot == fs::canonical(fixture.scratch.path, error)
            && !error,
        "The descriptor's trustedScratchRoot did not match the caller-provided root");
    require(ManifestProposalOverlay::cleanup(overlay.value()).success(), "Cleanup failed");
}

// #5: an unsafe (reparse-point) scratch root is rejected before any
// candidate directory is created under it.
void testReparsePointScratchRootIsRejected()
{
    Fixture fixture;
    TempDir realScratch;
    std::error_code error;
    const auto linkedScratch = fixture.workspace.path.parent_path() / "reparse-scratch-link";
    fs::create_directory_symlink(realScratch.path, linkedScratch, error);
    if (error) return; // No symlink privilege in this environment - nothing to assert.
    auto request = overlayRequest(fixture, {});
    request.trustedScratchRoot = linkedScratch;
    const auto result = ManifestProposalOverlay::build(request, fixture.service);
    require(!result.success() && result.error()->code == "overlay.invalid_scratch_root",
        "A reparse-point scratch root was accepted");
}

// #6: a candidate path that already exceeds the Windows path budget before
// any CMake command runs must fail closed with verification.
// overlay_path_too_long - the FakeCommandRunner records zero calls,
// proving the preflight runs strictly before dispatch. Uses a synthetic
// long root (a real overlay is not required - the preflight is pure string
// length, no filesystem access to the candidate path itself).
void testPathBudgetOverflowFailsClosedBeforeAnyCommand()
{
#ifdef _WIN32
    Fixture fixture;
    ManifestProposalOverlayDescriptor overlay;
    overlay.root = fs::path("C:/") / std::string(240, 'p');
    overlay.workspaceIdentity = "workspace-1";
    overlay.baselineFingerprint = std::string(64, 'a');
    overlay.overlayFingerprint = std::string(64, 'b');

    ExecutionAcceptanceRequirement build;
    build.id = "build";
    build.description = "Build succeeds";
    build.verificationKind = ExecutionVerificationKind::CMakeBuild;
    build.verificationArguments = {
        {"preset", "local-qt-debug"}, {"buildPreset", "local-qt-debug"}};
    ManifestProposalSetVerifierRequest request {overlay, {taskWithRequirement(build)}};
    FakeCommandRunner runner;
    const auto verification = ManifestProposalSetVerifier::verify(
        request, fixture.service, runner, {});
    require(verification.success(), "Path-budget overflow must not hard-fail verify() itself");
    require(runner.calls == 0,
        "A command was dispatched despite the candidate path exceeding its budget");
    const bool hasDiagnostic = std::any_of(
        verification.value().diagnostics.begin(), verification.value().diagnostics.end(),
        [](const ManifestVerificationDiagnostic& diagnostic) {
            return diagnostic.code == "verification.overlay_path_too_long";
        });
    require(hasDiagnostic, "Expected verification.overlay_path_too_long before any command ran");
    require(!requiredAcceptancePassed(verification.value()),
        "An overlay_path_too_long check must never count as passed");
#endif
}

// The path-shape closest to the real incident: a candidate root plus the
// exact observed relative binary directory
// ("build/Desktop_Qt_6_11_1_MSVC2022_64bit_Debug"), independent of any
// user project - this fixture is entirely self-contained.
void testRealisticQtBuildDirectoryShapeTripsPathBudget()
{
#ifdef _WIN32
    Fixture fixture;
    ManifestProposalOverlayDescriptor overlay;
    // 121 characters, matching the diagnosed candidate root length before
    // the fix, still under a short TEMP prefix so the test is host-
    // independent.
    overlay.root = fs::path("C:/Users/example-user/AppData/Local/Temp/"
        "ai-orchestrator-manifest-overlays/manifest-overlay-"
        "11111111-2222-3333-4444-555555555555");
    overlay.workspaceIdentity = "workspace-1";
    overlay.baselineFingerprint = std::string(64, 'a');
    overlay.overlayFingerprint = std::string(64, 'b');

    ExecutionAcceptanceRequirement lint;
    lint.id = "lint";
    lint.description = "QML lint succeeds";
    lint.verificationKind = ExecutionVerificationKind::QmlLint;
    lint.verificationArguments = {
        {"buildDirectory", "build/Desktop_Qt_6_11_1_MSVC2022_64bit_Debug"},
        {"target", "all_qmllint"}};
    ManifestProposalSetVerifierRequest request {overlay, {taskWithRequirement(lint)}};
    FakeCommandRunner runner;
    const auto verification = ManifestProposalSetVerifier::verify(
        request, fixture.service, runner, {});
    require(verification.success(), "Realistic path-shape must not hard-fail verify() itself");
    require(runner.calls == 0,
        "A command was dispatched despite the realistic candidate path exceeding its budget");
    const bool hasDiagnostic = std::any_of(
        verification.value().diagnostics.begin(), verification.value().diagnostics.end(),
        [](const ManifestVerificationDiagnostic& diagnostic) {
            return diagnostic.code == "verification.overlay_path_too_long";
        });
    require(hasDiagnostic,
        "The exact previously-diagnosed candidate path shape did not trip the path budget");
#endif
}

// #7: a successful (exit 0) configure whose output nonetheless contains
// CMake's own CMAKE_OBJECT_PATH_MAX marker must still fail closed with the
// same stable code - and the build step must never run.
void testCMakeObjectPathMaxMarkerStopsBeforeBuild()
{
    Fixture fixture;
    const auto overlay = ManifestProposalOverlay::build(
        overlayRequest(fixture, {}), fixture.service);
    require(overlay.success(), "Fixture overlay failed to build");

    ExecutionAcceptanceRequirement build;
    build.id = "build";
    build.description = "Build succeeds";
    build.verificationKind = ExecutionVerificationKind::CMakeBuild;
    build.verificationArguments = {
        {"preset", "local-qt-debug"}, {"buildPreset", "local-qt-debug"}};
    ManifestProposalSetVerifierRequest request {overlay.value(), {taskWithRequirement(build)}};

    FakeCommandRunner runner;
    ManifestVerificationCommandOutcome configureWithMarker;
    configureWithMarker.started = true;
    configureWithMarker.exitCode = 0;
    configureWithMarker.boundedOutput =
        "CMake Warning in CMakeLists.txt:\n  The object file directory\n\n"
        "    " + overlay.value().root.string() + "/CMakeFiles/x.dir/./\n\n"
        "  has 240 characters.  The maximum full path to an object file is 250\n"
        "  characters (see CMAKE_OBJECT_PATH_MAX).  Object file\n\n"
        "    some_target.obj\n\n  cannot be safely placed under this directory.\n";
    runner.outcomes.push_back(std::move(configureWithMarker));

    const auto verification = ManifestProposalSetVerifier::verify(
        request, fixture.service, runner, {});
    require(verification.success(), "CMAKE_OBJECT_PATH_MAX detection must not hard-fail verify()");
    require(runner.calls == 1,
        "The build step ran after configure reported CMAKE_OBJECT_PATH_MAX");
    const bool hasDiagnostic = std::any_of(
        verification.value().diagnostics.begin(), verification.value().diagnostics.end(),
        [](const ManifestVerificationDiagnostic& diagnostic) {
            return diagnostic.code == "verification.overlay_path_too_long";
        });
    require(hasDiagnostic,
        "A successful configure reporting CMAKE_OBJECT_PATH_MAX did not stop before build");
    require(!requiredAcceptancePassed(verification.value()),
        "CMAKE_OBJECT_PATH_MAX must never count as a passed required check");
    require(ManifestProposalOverlay::cleanup(overlay.value()).success(), "Cleanup failed");
}

// #8: synthetic noisy MSVC output - a long compiler invocation line
// (mentioning a "...\CMakeFiles\..." path, the exact old false-positive
// trigger) followed by the real "fatal error C1083" line. The excerpt must
// surface C1083, not the long command, and must never contain a raw
// absolute path.
void testNoisyMsvcOutputSurfacesFatalErrorNotCommandLine()
{
    Fixture fixture;
    const auto overlay = ManifestProposalOverlay::build(
        overlayRequest(fixture, {}), fixture.service);
    require(overlay.success(), "Fixture overlay failed to build");

    ExecutionAcceptanceRequirement build;
    build.id = "build";
    build.description = "Build succeeds";
    build.verificationKind = ExecutionVerificationKind::CMakeBuild;
    build.verificationArguments = {
        {"preset", "local-qt-debug"}, {"buildPreset", "local-qt-debug"}};
    ManifestProposalSetVerifierRequest request {overlay.value(), {taskWithRequirement(build)}};

    FakeCommandRunner runner;
    runner.outcomes.push_back(successfulProbe("configure ok"));
    ManifestVerificationCommandOutcome buildFailure;
    buildFailure.started = true;
    buildFailure.exitCode = 1;
    const std::string longCompileCommand =
        "C:\\Program Files\\Microsoft Visual Studio\\18\\Community\\VC\\Tools\\MSVC\\"
        "14.51.36231\\bin\\Hostx64\\x64\\cl.exe /c /I\""
        + overlay.value().root.string()
        + "\\build\\local-qt-debug\\CMakeFiles\\app.dir\" /Fo\""
        + overlay.value().root.string()
        + "\\build\\local-qt-debug\\CMakeFiles\\app.dir\\Main.cpp.obj\" /EHsc /MDd Main.cpp";
    buildFailure.boundedOutput = longCompileCommand
        + "\nMain.cpp\n"
        + overlay.value().root.string()
        + "\\Main.cpp(12): fatal error C1083: Cannot open compiler generated file: "
          "'Main.cpp.obj': Invalid argument\n";
    runner.outcomes.push_back(std::move(buildFailure));

    const auto verification = ManifestProposalSetVerifier::verify(
        request, fixture.service, runner, {});
    require(verification.success(), "Build failure must produce a typed verification result");
    const auto commandFailure = std::find_if(
        verification.value().diagnostics.begin(), verification.value().diagnostics.end(),
        [](const ManifestVerificationDiagnostic& diagnostic) {
            return diagnostic.code == "verification.command_failed";
        });
    require(commandFailure != verification.value().diagnostics.end(),
        "Expected a command_failed diagnostic for the build failure");
    require(commandFailure->message.find("C1083") != std::string::npos,
        "The diagnostic excerpt lost the real fatal error C1083");
    require(commandFailure->message.find("C:\\") == std::string::npos
            && commandFailure->message.find(overlay.value().root.string()) == std::string::npos,
        "The diagnostic excerpt leaked an absolute path");
    require(ManifestProposalOverlay::cleanup(overlay.value()).success(), "Cleanup failed");
}

// #9: a QML warning excerpt surfaces "Type name must be upper case" so a
// repair worker can see what to actually fix, not just that lint failed.
void testQmlWarningExcerptContainsTypeNameDiagnostic()
{
    Fixture fixture;
    fixture.add("p1", {change("a", FileChangeOperation::Create, "sidebar.qml", "clean")});
    const auto overlay = ManifestProposalOverlay::build(
        overlayRequest(fixture, {"p1"}), fixture.service);
    require(overlay.success(), "Fixture overlay failed to build");

    ExecutionAcceptanceRequirement lint;
    lint.id = "lint";
    lint.description = "QML lint succeeds";
    lint.verificationKind = ExecutionVerificationKind::QmlLint;
    lint.verificationArguments = {
        {"buildDirectory", "build"}, {"target", "all_qmllint"}};
    ManifestProposalSetVerifierRequest request {overlay.value(), {taskWithRequirement(lint)}};

    FakeCommandRunner runner;
    ManifestVerificationCommandOutcome warningOutcome;
    warningOutcome.started = true;
    warningOutcome.exitCode = 0;
    warningOutcome.boundedOutput =
        "sidebar.qml:3:1: warning: Type name must be upper case [syntax]\n";
    runner.outcomes.push_back(std::move(warningOutcome));

    const auto verification = ManifestProposalSetVerifier::verify(
        request, fixture.service, runner, {});
    require(verification.success(), "QML warning must produce a typed verification result");
    const auto qmlWarning = std::find_if(
        verification.value().diagnostics.begin(), verification.value().diagnostics.end(),
        [](const ManifestVerificationDiagnostic& diagnostic) {
            return diagnostic.code == "verification.qml_warning";
        });
    require(qmlWarning != verification.value().diagnostics.end(),
        "Expected a verification.qml_warning diagnostic");
    require(qmlWarning->message.find("Type name must be upper case") != std::string::npos,
        "QML warning diagnostic lost its leak-safe excerpt");
    require(ManifestProposalOverlay::cleanup(overlay.value()).success(), "Cleanup failed");
}

// Runs one QmlLint verification with the given exit-0, warning-bearing
// boundedOutput and returns the resulting verification.qml_warning
// diagnostic's message (the leak-safe excerpt) - shared by the bounded
// diagnostic handoff tests below.
std::string qmlLintExcerptFor(const std::string& boundedOutput)
{
    Fixture fixture;
    fixture.add("p1", {change("a", FileChangeOperation::Create, "sidebar.qml", "clean")});
    const auto overlay = ManifestProposalOverlay::build(
        overlayRequest(fixture, {"p1"}), fixture.service);
    require(overlay.success(), "Fixture overlay failed to build");

    ExecutionAcceptanceRequirement lint;
    lint.id = "lint";
    lint.description = "QML lint succeeds";
    lint.verificationKind = ExecutionVerificationKind::QmlLint;
    lint.verificationArguments = {{"buildDirectory", "build"}, {"target", "all_qmllint"}};
    ManifestProposalSetVerifierRequest request {overlay.value(), {taskWithRequirement(lint)}};

    FakeCommandRunner runner;
    ManifestVerificationCommandOutcome warningOutcome;
    warningOutcome.started = true;
    warningOutcome.exitCode = 0;
    warningOutcome.boundedOutput = boundedOutput;
    runner.outcomes.push_back(std::move(warningOutcome));

    const auto verification = ManifestProposalSetVerifier::verify(
        request, fixture.service, runner, {});
    require(verification.success(), "QML warning must produce a typed verification result");
    const auto qmlWarning = std::find_if(
        verification.value().diagnostics.begin(), verification.value().diagnostics.end(),
        [](const ManifestVerificationDiagnostic& diagnostic) {
            return diagnostic.code == "verification.qml_warning";
        });
    require(qmlWarning != verification.value().diagnostics.end(),
        "Expected a verification.qml_warning diagnostic");
    require(ManifestProposalOverlay::cleanup(overlay.value()).success(), "Cleanup failed");
    return qmlWarning->message;
}

// Sibling of qmlLintExcerptFor returning the same diagnostic's structured
// findings instead of its flat message - both are populated from the exact
// same selected/sanitized lines inside leakSafeDiagnosticExcerpt, so any
// coverage guarantee proven for the flat excerpt (every affected file
// represented, etc.) must hold identically here.
std::vector<ManifestVerificationDiagnosticFinding> qmlLintFindingsFor(
    const std::string& boundedOutput)
{
    Fixture fixture;
    fixture.add("p1", {change("a", FileChangeOperation::Create, "sidebar.qml", "clean")});
    const auto overlay = ManifestProposalOverlay::build(
        overlayRequest(fixture, {"p1"}), fixture.service);
    require(overlay.success(), "Fixture overlay failed to build");

    ExecutionAcceptanceRequirement lint;
    lint.id = "lint";
    lint.description = "QML lint succeeds";
    lint.verificationKind = ExecutionVerificationKind::QmlLint;
    lint.verificationArguments = {{"buildDirectory", "build"}, {"target", "all_qmllint"}};
    ManifestProposalSetVerifierRequest request {overlay.value(), {taskWithRequirement(lint)}};

    FakeCommandRunner runner;
    ManifestVerificationCommandOutcome warningOutcome;
    warningOutcome.started = true;
    warningOutcome.exitCode = 0;
    warningOutcome.boundedOutput = boundedOutput;
    runner.outcomes.push_back(std::move(warningOutcome));

    const auto verification = ManifestProposalSetVerifier::verify(
        request, fixture.service, runner, {});
    require(verification.success(), "QML warning must produce a typed verification result");
    const auto qmlWarning = std::find_if(
        verification.value().diagnostics.begin(), verification.value().diagnostics.end(),
        [](const ManifestVerificationDiagnostic& diagnostic) {
            return diagnostic.code == "verification.qml_warning";
        });
    require(qmlWarning != verification.value().diagnostics.end(),
        "Expected a verification.qml_warning diagnostic");
    require(ManifestProposalOverlay::cleanup(overlay.value()).success(), "Cleanup failed");
    return qmlWarning->structuredFindings;
}

// Real-Run incident (Run 8081057a-a669-4f58-abec-952244624d74, 2026-09-11):
// the structured findings a repair prompt groups by file are parsed from
// the exact same selected lines the flat excerpt already covers - so the
// same "every affected file represented" guarantee already proven for the
// excerpt text (testBoundedExcerptCoversMultipleFiles) must hold here too,
// with correct per-finding line/column/tag extraction.
void testStructuredFindingsCoverEveryAffectedFileWithCorrectLocation()
{
    const auto findings = qmlLintFindingsFor(
        "AppSidebar.qml:3:1: warning: Type name must be upper case [syntax]\n"
        "AppTopBar.qml:7:5: warning: Type name must be upper case [syntax]\n");
    require(findings.size() == 2, "Expected one structured finding per affected file");
    std::unordered_set<std::string> files;
    for (const auto& finding : findings) files.insert(finding.fileKey);
    require(files.count("AppSidebar.qml") == 1 && files.count("AppTopBar.qml") == 1,
        "Structured findings dropped one affected file");
    const auto sidebar = std::find_if(findings.begin(), findings.end(),
        [](const auto& finding) { return finding.fileKey == "AppSidebar.qml"; });
    require(sidebar != findings.end() && sidebar->line == 3 && sidebar->column == 1
            && sidebar->tag == "syntax",
        "Structured finding lost its exact line/column/tag");
}

// Real-Run incident (Run bb56e001-eac0-4b59-8d0b-d037c6bc1031, 2026-09-15,
// surfaced via MW-P5.4's "Continue from failed verification"): qmllint
// emitted a CONTINUATION line for the diagnostic immediately above it -
// repeating the SAME line:column but never repeating the file path (a real
// two-line qmllint module-import warning). diagnosticLineFileKey's leading
// ":<digit>" scan matched that bare "4:1:" shape too, producing a bogus
// structured finding whose fileKey was just the digit string "4" - which
// then failed ManifestVerificationAttributionService's candidate-relative-
// path check and permanently blocked (the attribution row is immutable)
// that Run's continuation. A continuation line must never produce its own
// structured finding at all - only the genuine file-owning line above it.
void testStructuredFindingsSkipFilelessContinuationLine()
{
    const auto findings = qmlLintFindingsFor(
        "Warning: AppCard.qml:4:1: Warnings occurred while importing module "
        "\"NewDashboard_UI_IHK_Project\": [import]\n"
        "Warning: 4:1: Failed to import NewDashboard_UI_IHK_Project. Are your "
        "import paths set up properly? [import]\n");
    require(findings.size() == 1,
        "A fileless continuation line must never become its own structured finding");
    require(findings.front().fileKey == "AppCard.qml" && findings.front().line == 4
            && findings.front().column == 1,
        "The one real finding must still carry the genuine file's line/column");
}

// Real-Run incident (Run bb56e001-eac0-4b59-8d0b-d037c6bc1031, 2026-09-15,
// surfaced via MW-P5.4's "Continue from failed verification"): qmllint's
// "Info: " severity prefix (its lowest severity, e.g. unused-import notices)
// was missing from stripDiagnosticWrapperPrefix's wrapper list, leaving it
// attached to the parsed fileKey and failing
// ManifestVerificationAttributionService's candidate-relative-path check.
void testStructuredFindingsStripInfoSeverityPrefix()
{
    // A real qmllint run never fails on an Info-only line alone; paired here
    // with a genuine Warning so the check's overall status is Failed and a
    // verification.qml_warning diagnostic is actually produced, exactly like
    // the real bb56e001 output (which mixed both severities together).
    const auto findings = qmlLintFindingsFor(
        "Info: AppShell.qml:2:1: Unused import [unused-imports]\n"
        "AppSidebar.qml:33:24: warning: Unqualified access [unqualified]\n");
    require(findings.size() == 2, "Both the Info and Warning lines must become structured findings");
    const auto info = std::find_if(findings.begin(), findings.end(),
        [](const auto& finding) { return finding.line == 2; });
    require(info != findings.end() && info->fileKey == "AppShell.qml",
        "The \"Info: \" severity prefix must be stripped from the fileKey, same as Warning/Error/Note");
}

// Real-Run incident (Run fea29bc8-3f5e-4d05-a511-ecc2475cde5b,
// 2026-09-23): Qt 6.11 concatenated qmllint's explanatory hint and the next
// Warning line without a newline. The old parser treated the entire hint as
// part of fileKey, so the otherwise-valid retained Run was permanently
// blocked by verification_continuation.attribution_path_invalid when the
// user selected Create repair workspace. Recover the known
// "<severity>: <candidate>/" boundary without accepting arbitrary prefixed
// paths.
void testStructuredFindingsRecoverDiagnosticAfterConcatenatedQmlLintHint()
{
    const auto findings = qmlLintFindingsFor(
        "Warning: <candidate>/qml/CanaryPanel.qml:95:31: Unqualified access "
        "[unqualified]\n"
        "      You first have to give the element an idWarning: "
        "<candidate>/qml/CanaryPanel.qml:96:44: Unqualified access [unqualified]\n");
    require(findings.size() == 2,
        "The concatenated qmllint hint must not discard or duplicate either finding");
    require(findings[0].fileKey == "<candidate>/qml/CanaryPanel.qml"
            && findings[0].line == 95 && findings[0].column == 31,
        "The ordinary diagnostic must retain its exact candidate-relative provenance");
    require(findings[1].fileKey == "<candidate>/qml/CanaryPanel.qml"
            && findings[1].line == 96 && findings[1].column == 44
            && findings[1].tag == "unqualified",
        "The diagnostic after the concatenated hint was not recovered safely");
}

// A line with no "<file>:<line>:<column>:" location shape (e.g. an
// unattributed warning) must never be force-fit into a structured finding -
// it stays covered only by the flat excerpt text, exactly as before this
// change.
void testStructuredFindingsEmptyWhenLineHasNoLocationShape()
{
    const auto findings = qmlLintFindingsFor(
        "Warning: diagnostic source unavailable [unqualified]\n");
    require(findings.empty(),
        "A location-less line must not be force-parsed into a structured finding");
}

// #23: actionable warnings from at least two different files both survive
// the bounded excerpt - the real incident this closes lost AppSidebar.qml's
// own warning behind only the LAST tier lines, which happened to belong to
// a different, later file.
void testBoundedExcerptCoversMultipleFiles()
{
    const auto excerpt = qmlLintExcerptFor(
        "AppSidebar.qml:3:1: warning: Type name must be upper case [syntax]\n"
        "AppTopBar.qml:7:1: warning: Type name must be upper case [syntax]\n");
    require(excerpt.find("AppSidebar.qml") != std::string::npos,
        "Bounded excerpt dropped the first file's actionable warning");
    require(excerpt.find("AppTopBar.qml") != std::string::npos,
        "Bounded excerpt dropped the second file's actionable warning");
}

// Real-Run incident (Run c573539c-ddf1-4ded-bd70-a60d2ba475a7, 2026-09-11):
// three affected files whose combined actionable line count exceeds the
// 12-line cap, with the middle file's warnings positioned entirely in what
// the OLD plain head/tail-by-LINE-POSITION selection would have dropped
// (AppShell fills the first 6 lines, AppSidebar the next 4, AppTopBar the
// last 6 - 16 total, so the old "first 6 + last 6" rule kept none of
// AppSidebar's 4 middle lines at all). The repair worker in the real
// incident then "fixed" only the two files it could see and left
// AppSidebar's warnings completely unaddressed, failing the very next
// verification. Every one of the three files must now be represented.
void testBoundedExcerptGuaranteesEveryFileWhenMiddleClusterWouldBeDropped()
{
    std::string output;
    for (int i = 0; i < 6; ++i) {
        output += "AppShell.qml:" + std::to_string(10 + i)
            + ":1: warning: unqualified access " + std::to_string(i) + " [unqualified]\n";
    }
    for (int i = 0; i < 4; ++i) {
        output += "AppSidebar.qml:" + std::to_string(20 + i)
            + ":1: warning: unqualified access " + std::to_string(i) + " [unqualified]\n";
    }
    for (int i = 0; i < 6; ++i) {
        output += "AppTopBar.qml:" + std::to_string(30 + i)
            + ":1: warning: unqualified access " + std::to_string(i) + " [unqualified]\n";
    }
    const auto excerpt = qmlLintExcerptFor(output);
    require(excerpt.find("AppShell.qml") != std::string::npos,
        "Bounded excerpt dropped the first file's actionable warning");
    require(excerpt.find("AppSidebar.qml") != std::string::npos,
        "Bounded excerpt dropped the MIDDLE file's actionable warning entirely - exactly the "
        "real incident this closes, where a repair worker had zero evidence for a file it had "
        "equally broken");
    require(excerpt.find("AppTopBar.qml") != std::string::npos,
        "Bounded excerpt dropped the last file's actionable warning");
    require(excerpt.find("truncated") != std::string::npos,
        "A genuinely truncated excerpt must still carry the truncation marker");
}

// #24/#28: when the deduplicated actionable line count exceeds the cap,
// both the earliest and the latest findings remain represented, and a
// deterministic truncation marker is appended.
void testBoundedExcerptRepresentsEarlyAndLateLinesWithTruncationMarker()
{
    std::string output;
    for (int i = 0; i < 20; ++i) {
        output += "File" + std::to_string(i)
            + ".qml:1:1: warning: unique diagnostic " + std::to_string(i) + " [unqualified]\n";
    }
    const auto excerpt = qmlLintExcerptFor(output);
    require(excerpt.find("diagnostic 0") != std::string::npos,
        "The earliest actionable finding must survive the bounded cap");
    require(excerpt.find("diagnostic 19") != std::string::npos,
        "The latest actionable finding must survive the bounded cap");
    require(excerpt.find("diagnostic 10") == std::string::npos,
        "A middle finding beyond the deterministic head/tail selection must not silently appear");
    require(excerpt.find("truncated") != std::string::npos,
        "A truncated excerpt must carry the deterministic truncation marker");
}

// #25: an identical diagnostic line repeated many times must never consume
// more than one of the bounded slots - deduplication, not just truncation,
// keeps the excerpt informative.
void testBoundedExcerptDeduplicatesRepeatedLines()
{
    std::string output;
    for (int i = 0; i < 20; ++i) {
        output += "AppSidebar.qml:3:1: warning: Type name must be upper case [syntax]\n";
    }
    const auto excerpt = qmlLintExcerptFor(output);
    require(excerpt.find("Type name must be upper case") != std::string::npos,
        "The repeated diagnostic must still be represented");
    require(excerpt.find(" | ") == std::string::npos,
        "20 duplicate lines must deduplicate down to a single entry, not fill the whole cap");
    require(excerpt.find("truncated") == std::string::npos,
        "Deduplicating 20 identical lines down to one must never itself trigger truncation");
}

// #27: an absolute path embedded inside an otherwise-actionable line is
// still redacted, and a secret-like line is still dropped entirely - the
// new multi-line/multi-file selection logic must not bypass either
// pre-existing leak-safe filter.
void testBoundedExcerptStillFiltersAbsolutePathsAndSecrets()
{
    const auto excerpt = qmlLintExcerptFor(
        "authorization: Bearer sk-should-never-appear\n"
        "C:\\Users\\example\\project\\AppSidebar.qml:3:1: warning: Type name must be "
        "upper case [syntax]\n");
    require(excerpt.find("Type name must be upper case") != std::string::npos,
        "The actionable warning itself must still survive filtering");
    require(excerpt.find("<absolute-path>") != std::string::npos,
        "An absolute path inside an actionable line must be redacted, not passed through raw");
    require(excerpt.find("C:\\Users\\example") == std::string::npos,
        "The raw absolute path must never appear in the excerpt");
    require(excerpt.find("sk-should-never-appear") == std::string::npos,
        "A secret-like line must be dropped entirely, never partially surfaced");
}

// #26: a real fatal error still displaces long command-line noise even
// under the new multi-line selection logic - regression companion to
// testNoisyMsvcOutputSurfacesFatalErrorNotCommandLine, using a build with
// SEVERAL noisy command lines (not just one) to prove the tiering, not
// just the old single-candidate path, still wins.
void testBoundedExcerptPrioritizesFatalErrorOverMultipleNoisyCommandLines()
{
    Fixture fixture;
    const auto overlay = ManifestProposalOverlay::build(
        overlayRequest(fixture, {}), fixture.service);
    require(overlay.success(), "Fixture overlay failed to build");

    ExecutionAcceptanceRequirement build;
    build.id = "build";
    build.description = "Build succeeds";
    build.verificationKind = ExecutionVerificationKind::CMakeBuild;
    build.verificationArguments = {
        {"preset", "local-qt-debug"}, {"buildPreset", "local-qt-debug"}};
    ManifestProposalSetVerifierRequest request {overlay.value(), {taskWithRequirement(build)}};

    FakeCommandRunner runner;
    runner.outcomes.push_back(successfulProbe("configure ok"));
    ManifestVerificationCommandOutcome buildFailure;
    buildFailure.started = true;
    buildFailure.exitCode = 1;
    std::string noisy;
    for (int i = 0; i < 10; ++i) {
        noisy += "cl.exe /c /I\"" + overlay.value().root.string()
            + "\\build\\local-qt-debug\\CMakeFiles\\app" + std::to_string(i) + ".dir\" /EHsc /MDd Main"
            + std::to_string(i) + ".cpp\n";
    }
    buildFailure.boundedOutput = noisy
        + overlay.value().root.string()
        + "\\Main.cpp(12): fatal error C1083: Cannot open compiler generated file: "
          "'Main.cpp.obj': Invalid argument\n";
    runner.outcomes.push_back(std::move(buildFailure));

    const auto verification = ManifestProposalSetVerifier::verify(
        request, fixture.service, runner, {});
    require(verification.success(), "Build failure must produce a typed verification result");
    const auto commandFailure = std::find_if(
        verification.value().diagnostics.begin(), verification.value().diagnostics.end(),
        [](const ManifestVerificationDiagnostic& diagnostic) {
            return diagnostic.code == "verification.command_failed";
        });
    require(commandFailure != verification.value().diagnostics.end(),
        "Expected a command_failed diagnostic for the build failure");
    require(commandFailure->message.find("C1083") != std::string::npos,
        "Ten noisy command lines must never displace the real fatal error");
    require(commandFailure->message.find("cl.exe") == std::string::npos,
        "Low-value command-line noise must not appear in the excerpt at all");
    require(ManifestProposalOverlay::cleanup(overlay.value()).success(), "Cleanup failed");
}

// Hardening follow-up #1a: a tier-1 FAILED: marker must never exclude a
// genuinely different, equally actionable tier-2 QML diagnostic found in
// the SAME output - both are "significant" and must both survive.
void testBoundedExcerptCombinesFatalAndQmlDiagnosticTiers()
{
    const auto excerpt = qmlLintExcerptFor(
        "FAILED: [code=1] ninja target build failed\n"
        "AppSidebar.qml:3:1: warning: Type name must be upper case [syntax]\n");
    require(excerpt.find("FAILED:") != std::string::npos,
        "The tier-1 command-failure marker must still appear");
    require(excerpt.find("Type name must be upper case") != std::string::npos,
        "A tier-1 FAILED: line must never displace a genuinely different tier-2 QML diagnostic");
}

// Hardening follow-up #1b: the excerpt (including any appended truncation
// marker) must never exceed the byte cap, even when truncation is
// discovered only while appending bytes (every selected line already fits
// under the LINE-count cap, but their combined length still exceeds the
// BYTE cap) - the exact case the previous pre-reservation-only logic could
// still overrun.
void testBoundedExcerptNeverExceedsByteCapEvenWithMarker()
{
    std::string output;
    const std::string padding(220, 'x');
    // 10 unique lines (well under the 12-line cap) whose combined length
    // comfortably exceeds the 2048-byte cap.
    for (int i = 0; i < 10; ++i) {
        output += "File" + std::to_string(i)
            + ".qml:1:1: warning: Type name must be upper case " + padding + " [syntax]\n";
    }
    const auto message = qmlLintExcerptFor(output);
    // qmlLintExcerptFor returns the full diagnostic MESSAGE (a fixed,
    // untruncated prefix + leakSafeDiagnosticExcerpt's own return value) -
    // the 2048-byte cap is a contract on the excerpt itself, not on the
    // message that embeds it, so isolate exactly the embedded substring.
    const std::string prefix = "; diagnostic excerpt: ";
    const auto prefixPos = message.find(prefix);
    require(prefixPos != std::string::npos, "Expected the diagnostic-excerpt prefix in the message");
    const auto excerpt = message.substr(prefixPos + prefix.size());
    require(excerpt.size() <= 2048,
        "The excerpt, including any truncation marker, must never exceed the declared byte cap");
    require(excerpt.find("truncated") != std::string::npos,
        "A byte-cap-only truncation (line count already within its own cap) must still carry "
        "the marker");
}

// Verification-repair reliability hardening item #5 (import-warning
// handling): confirms, rather than assumes, that the QML lint command's
// module/import environment is the candidate's own. The overlay is a full
// self-consistent copy of the whole baseline plus every proposed change
// (see ManifestProposalOverlay::build), and configure/build/lint all run
// with workingDirectory == the SAME overlay root - so a candidate's own
// module-registration/import changes are exactly what qmllint resolves
// against, never the real project workspace's. No overlay/import-path
// defect was found; this is a regression test for that conclusion, not a
// behavior change.
void testQmlLintCommandRunsAgainstCandidateOverlayNotRealWorkspace()
{
    Fixture fixture;
    fixture.add("p1", {change("a", FileChangeOperation::Create,
        "qml/components/navigation/AppSidebar.qml", "Item {}")});
    const auto overlay = ManifestProposalOverlay::build(
        overlayRequest(fixture, {"p1"}), fixture.service);
    require(overlay.success(), "Fixture overlay failed to build");

    ExecutionAcceptanceRequirement lint;
    lint.id = "lint";
    lint.description = "QML lint completes without warnings";
    lint.verificationKind = ExecutionVerificationKind::QmlLint;
    lint.verificationArguments = {{"buildDirectory", "build"}, {"target", "all_qmllint"}};
    ManifestProposalSetVerifierRequest request {overlay.value(), {taskWithRequirement(lint)}};

    FakeCommandRunner runner;
    ManifestVerificationCommandOutcome cleanOutcome;
    cleanOutcome.started = true;
    cleanOutcome.exitCode = 0;
    cleanOutcome.boundedOutput = "Lint passed with no warnings.\n";
    runner.outcomes.push_back(cleanOutcome);

    const auto verification = ManifestProposalSetVerifier::verify(
        request, fixture.service, runner, {});
    require(verification.success(), "Verification call itself failed");
    require(runner.requests.size() == 1, "Expected exactly one qml_lint command dispatch");
    require(runner.requests.front().workingDirectory == overlay.value().root,
        "The qml_lint command must run against the candidate overlay root - the same root "
        "configure/build already ran against - so its resolved module/import environment "
        "reflects the candidate's own proposed changes");
    require(runner.requests.front().workingDirectory != fixture.workspace.path,
        "The qml_lint command must never run directly against the real project workspace");
}

// #30: a candidate-introduced module/import warning is an ordinary QmlLint
// failure like any other - it is never specially suppressed or downgraded
// merely because its text concerns an import/module resolution problem
// rather than a plain lint rule.
void testCandidateModuleImportWarningStillFailsVerification()
{
    Fixture fixture;
    fixture.add("p1", {change("a", FileChangeOperation::Create,
        "qml/components/navigation/AppSidebar.qml", "import \"./AppSidebar\"\nItem {}")});
    const auto overlay = ManifestProposalOverlay::build(
        overlayRequest(fixture, {"p1"}), fixture.service);
    require(overlay.success(), "Fixture overlay failed to build");

    ExecutionAcceptanceRequirement lint;
    lint.id = "lint";
    lint.description = "QML lint completes without warnings";
    lint.verificationKind = ExecutionVerificationKind::QmlLint;
    lint.verificationArguments = {{"buildDirectory", "build"}, {"target", "all_qmllint"}};
    ManifestProposalSetVerifierRequest request {overlay.value(), {taskWithRequirement(lint)}};

    FakeCommandRunner runner;
    ManifestVerificationCommandOutcome importWarning;
    importWarning.started = true;
    importWarning.exitCode = 0;
    importWarning.boundedOutput =
        "AppSidebar.qml:1:1: warning: Module \"AppSidebar\" is imported by itself "
        "[unqualified]\n";
    runner.outcomes.push_back(importWarning);

    const auto verification = ManifestProposalSetVerifier::verify(
        request, fixture.service, runner, {});
    require(verification.success(), "Verification call itself failed");
    const auto& check = verification.value().checks.front();
    require(check.status == ExecutionVerificationStatus::Failed,
        "A candidate-introduced module/import warning must still be an ordinary QmlLint failure");
    const auto commandFailure = std::find_if(check.diagnostics.begin(), check.diagnostics.end(),
        [](const ManifestVerificationDiagnostic& diagnostic) {
            return diagnostic.code == "verification.qml_warning";
        });
    require(commandFailure != check.diagnostics.end()
            && commandFailure->message.find("imported by itself") != std::string::npos,
        "The import-specific diagnostic text must survive into the reported finding, "
        "not be discarded or replaced with a generic message");
}

void testQmlWarningsOutsideFrozenTaskWriteScopeDoNotBlockTaskAcceptance()
{
    Fixture fixture;
    fixture.add("p1", {change("a", FileChangeOperation::Create,
        "qml/AppShell.qml", "Item {}")});
    const auto overlay = ManifestProposalOverlay::build(
        overlayRequest(fixture, {"p1"}), fixture.service);
    require(overlay.success(), "Fixture overlay failed to build");

    ExecutionAcceptanceRequirement lint;
    lint.id = "lint";
    lint.description = "Owned QML files lint without warnings";
    lint.verificationKind = ExecutionVerificationKind::QmlLint;
    lint.verificationArguments = {{"buildDirectory", "build"}, {"target", "all_qmllint"}};
    auto task = taskWithRequirement(lint);
    task.ownedPaths = {"qml/AppShell.qml"};
    ManifestProposalSetVerifierRequest request {overlay.value(), {std::move(task)}};

    FakeCommandRunner runner;
    ManifestVerificationCommandOutcome warning;
    warning.started = true;
    warning.exitCode = 0;
    warning.boundedOutput =
        "Warning: <candidate>/qml/Main.qml:3:1: Warnings occurred while importing module [import]\n"
        "Warning: 3:1: Failed to import module [import]\n"
        "Warning: <candidate>/qml/app/AppState.qml:3:1: Unused import [import]\n";
    runner.outcomes.push_back(std::move(warning));

    const auto verification = ManifestProposalSetVerifier::verify(
        request, fixture.service, runner, {});
    require(verification.success(), "Verification call itself failed");
    require(verification.value().checks.front().status == ExecutionVerificationStatus::Passed,
        "Warnings confined to files outside the task's frozen write scope blocked acceptance");
    require(verification.value().checks.front().diagnostics.size() == 1
            && verification.value().checks.front().diagnostics.front().code
                == "verification.qml_warning_outside_task_scope",
        "Out-of-scope QML warnings were not recorded with the bounded typed diagnostic");
    const auto& findings =
        verification.value().checks.front().diagnostics.front().structuredFindings;
    require(findings.size() == 2
            && findings.front().fileKey == "<candidate>/qml/Main.qml"
            && findings.back().fileKey == "<candidate>/qml/app/AppState.qml",
        "A passed outside-scope QML diagnostic must retain bounded structured provenance");
}

void testQmlWarningInFrozenTaskWriteScopeStillFails()
{
    Fixture fixture;
    fixture.add("p1", {change("a", FileChangeOperation::Create,
        "qml/AppShell.qml", "Item {}")});
    const auto overlay = ManifestProposalOverlay::build(
        overlayRequest(fixture, {"p1"}), fixture.service);
    require(overlay.success(), "Fixture overlay failed to build");

    ExecutionAcceptanceRequirement lint;
    lint.id = "lint";
    lint.description = "Owned QML files lint without warnings";
    lint.verificationKind = ExecutionVerificationKind::QmlLint;
    lint.verificationArguments = {{"buildDirectory", "build"}, {"target", "all_qmllint"}};
    auto task = taskWithRequirement(lint);
    task.ownedPaths = {"qml/AppShell.qml"};
    ManifestProposalSetVerifierRequest request {overlay.value(), {std::move(task)}};

    FakeCommandRunner runner;
    ManifestVerificationCommandOutcome warning;
    warning.started = true;
    warning.exitCode = 0;
    warning.boundedOutput =
        "Warning: <candidate>/qml/Main.qml:3:1: Unused import [import]\n"
        "Warning: <candidate>/qml/AppShell.qml:126:47: Unqualified access [unqualified]\n";
    runner.outcomes.push_back(std::move(warning));

    const auto verification = ManifestProposalSetVerifier::verify(
        request, fixture.service, runner, {});
    require(verification.success(), "Verification call itself failed");
    require(verification.value().checks.front().status == ExecutionVerificationStatus::Failed,
        "An in-scope QML warning was incorrectly ignored");
}

void testQmlWarningsOutsideFrozenDependencyOutputScopeDoNotBlockIntegrationTask()
{
    Fixture fixture;
    fixture.add("p1", {change("a", FileChangeOperation::Create,
        "qml/components/common/AppCard.qml", "Item {}")});
    const auto overlay = ManifestProposalOverlay::build(
        overlayRequest(fixture, {"p1"}), fixture.service);
    require(overlay.success(), "Fixture overlay failed to build");

    ExecutionManifestTask producer;
    producer.id = "components";
    producer.plannedCreatePaths = {"qml/components/common/AppCard.qml"};

    ExecutionAcceptanceRequirement lint;
    lint.id = "lint";
    lint.description = "Dependency QML outputs lint without warnings";
    lint.verificationKind = ExecutionVerificationKind::QmlLint;
    lint.verificationArguments = {{"buildDirectory", "build"}, {"target", "all_qmllint"}};
    auto integration = taskWithRequirement(lint);
    integration.id = "integration";
    integration.ownedPaths = {"qml/CMakeLists.txt"};
    integration.requiredReadPaths = {"qml/components/common/AppCard.qml"};
    integration.dependencies = {"components"};
    ManifestProposalSetVerifierRequest request {
        overlay.value(), {std::move(producer), std::move(integration)}};

    FakeCommandRunner runner;
    ManifestVerificationCommandOutcome warning;
    warning.started = true;
    warning.exitCode = 0;
    warning.boundedOutput =
        "Warning: <candidate>/qml/components/navigation/AppSidebar.qml:33:24: "
        "Unqualified access [unqualified]\n";
    runner.outcomes.push_back(std::move(warning));

    const auto verification = ManifestProposalSetVerifier::verify(
        request, fixture.service, runner, {});
    require(verification.success(), "Verification call itself failed");
    const auto& check = verification.value().checks.front();
    require(check.status == ExecutionVerificationStatus::Passed,
        "A warning outside the integration task's frozen dependency-output scope blocked it");
    require(check.diagnostics.size() == 1
            && check.diagnostics.front().code
                == "verification.qml_warning_outside_task_scope",
        "The outside-dependency-scope warning lost its bounded typed diagnostic");
    require(check.diagnostics.front().structuredFindings.size() == 1
            && check.diagnostics.front().structuredFindings.front().fileKey
                == "<candidate>/qml/components/navigation/AppSidebar.qml",
        "Outside dependency-scope evidence must remain available for baseline attribution");
}

void testQmlWarningInFrozenDependencyOutputScopeStillFailsIntegrationTask()
{
    Fixture fixture;
    fixture.add("p1", {change("a", FileChangeOperation::Create,
        "qml/components/common/AppCard.qml", "Item {}")});
    const auto overlay = ManifestProposalOverlay::build(
        overlayRequest(fixture, {"p1"}), fixture.service);
    require(overlay.success(), "Fixture overlay failed to build");

    ExecutionManifestTask producer;
    producer.id = "components";
    producer.plannedCreatePaths = {"qml/components/common/AppCard.qml"};

    ExecutionAcceptanceRequirement lint;
    lint.id = "lint";
    lint.description = "Dependency QML outputs lint without warnings";
    lint.verificationKind = ExecutionVerificationKind::QmlLint;
    lint.verificationArguments = {{"buildDirectory", "build"}, {"target", "all_qmllint"}};
    auto integration = taskWithRequirement(lint);
    integration.id = "integration";
    integration.ownedPaths = {"qml/CMakeLists.txt"};
    integration.requiredReadPaths = {"qml/components/common/AppCard.qml"};
    integration.dependencies = {"components"};
    ManifestProposalSetVerifierRequest request {
        overlay.value(), {std::move(producer), std::move(integration)}};

    FakeCommandRunner runner;
    ManifestVerificationCommandOutcome warning;
    warning.started = true;
    warning.exitCode = 0;
    warning.boundedOutput =
        "Warning: <candidate>/qml/components/common/AppCard.qml:9:20: "
        "Member contentData shadows a final member [property-override]\n";
    runner.outcomes.push_back(std::move(warning));

    const auto verification = ManifestProposalSetVerifier::verify(
        request, fixture.service, runner, {});
    require(verification.success(), "Verification call itself failed");
    require(verification.value().checks.front().status
            == ExecutionVerificationStatus::Failed,
        "A warning in a frozen dependency output was incorrectly ignored");
}

void testBaselineOnlyQmlReadDoesNotCreateVerificationScope()
{
    Fixture fixture;
    fixture.add("p1", {change("a", FileChangeOperation::Create,
        "qml/components/common/AppCard.qml", "Item {}")});
    const auto overlay = ManifestProposalOverlay::build(
        overlayRequest(fixture, {"p1"}), fixture.service);
    require(overlay.success(), "Fixture overlay failed to build");

    ExecutionAcceptanceRequirement lint;
    lint.id = "lint";
    lint.description = "Integration QML lint";
    lint.verificationKind = ExecutionVerificationKind::QmlLint;
    lint.verificationArguments = {{"buildDirectory", "build"}, {"target", "all_qmllint"}};
    auto integration = taskWithRequirement(lint);
    integration.id = "integration";
    integration.ownedPaths = {"qml/CMakeLists.txt"};
    integration.requiredReadPaths = {"qml/components/navigation/AppSidebar.qml"};
    integration.baselineOnlyReadPaths = {"qml/components/navigation/AppSidebar.qml"};
    ManifestProposalSetVerifierRequest request {overlay.value(), {std::move(integration)}};

    FakeCommandRunner runner;
    ManifestVerificationCommandOutcome warning;
    warning.started = true;
    warning.exitCode = 0;
    warning.boundedOutput =
        "Warning: <candidate>/qml/components/navigation/AppSidebar.qml:33:24: "
        "Unqualified access [unqualified]\n";
    runner.outcomes.push_back(std::move(warning));

    const auto verification = ManifestProposalSetVerifier::verify(
        request, fixture.service, runner, {});
    require(verification.success(), "Verification call itself failed");
    require(verification.value().checks.front().status
            == ExecutionVerificationStatus::Failed,
        "A baseline-only QML read must not become a verification authority boundary");
}

void testUnresolvedQmlReadDoesNotCreateVerificationScope()
{
    Fixture fixture;
    fixture.add("p1", {change("a", FileChangeOperation::Create,
        "qml/components/common/AppCard.qml", "Item {}")});
    const auto overlay = ManifestProposalOverlay::build(
        overlayRequest(fixture, {"p1"}), fixture.service);
    require(overlay.success(), "Fixture overlay failed to build");

    ExecutionAcceptanceRequirement lint;
    lint.id = "lint";
    lint.description = "Integration QML lint";
    lint.verificationKind = ExecutionVerificationKind::QmlLint;
    lint.verificationArguments = {{"buildDirectory", "build"}, {"target", "all_qmllint"}};
    auto integration = taskWithRequirement(lint);
    integration.id = "integration";
    integration.ownedPaths = {"qml/CMakeLists.txt"};
    integration.requiredReadPaths = {"qml/components/common/Unknown.qml"};
    ManifestProposalSetVerifierRequest request {overlay.value(), {std::move(integration)}};

    FakeCommandRunner runner;
    ManifestVerificationCommandOutcome warning;
    warning.started = true;
    warning.exitCode = 0;
    warning.boundedOutput =
        "Warning: <candidate>/qml/components/common/Unknown.qml:1:1: "
        "Unqualified access [unqualified]\n";
    runner.outcomes.push_back(std::move(warning));

    const auto verification = ManifestProposalSetVerifier::verify(
        request, fixture.service, runner, {});
    require(verification.success(), "Verification call itself failed");
    require(verification.value().checks.front().status
            == ExecutionVerificationStatus::Failed,
        "An unresolved required read must not widen the QML verification scope");
}

void testUnattributedQmlWarningStillFailsClosed()
{
    Fixture fixture;
    fixture.add("p1", {change("a", FileChangeOperation::Create,
        "qml/AppShell.qml", "Item {}")});
    const auto overlay = ManifestProposalOverlay::build(
        overlayRequest(fixture, {"p1"}), fixture.service);
    require(overlay.success(), "Fixture overlay failed to build");

    ExecutionAcceptanceRequirement lint;
    lint.id = "lint";
    lint.description = "Owned QML files lint without warnings";
    lint.verificationKind = ExecutionVerificationKind::QmlLint;
    lint.verificationArguments = {{"buildDirectory", "build"}, {"target", "all_qmllint"}};
    auto task = taskWithRequirement(lint);
    task.ownedPaths = {"qml/AppShell.qml"};
    ManifestProposalSetVerifierRequest request {overlay.value(), {std::move(task)}};

    FakeCommandRunner runner;
    ManifestVerificationCommandOutcome warning;
    warning.started = true;
    warning.exitCode = 0;
    warning.boundedOutput = "Warning: diagnostic source unavailable [unqualified]\n";
    runner.outcomes.push_back(std::move(warning));

    const auto verification = ManifestProposalSetVerifier::verify(
        request, fixture.service, runner, {});
    require(verification.success(), "Verification call itself failed");
    require(verification.value().checks.front().status == ExecutionVerificationStatus::Failed,
        "An unattributed QML warning must remain fail-closed");
}

// ---------------------------------------------------------------------
// Post-Stage-3 QML-lint hardening: resolveQmlLintCriterionDelta() pure unit
// coverage. Reuses computeQmlLintDiagnosticDelta()'s already-tested multiset
// resolver (see testFingerprintQmlLintFindingIgnoresLineAndColumn and
// testComputeQmlLintDiagnosticDeltaUsesMultisetSemanticsForDuplicateFingerprints
// above for that function's own relocation/duplicate-instance proof - not
// re-duplicated here) and adds only the ownership-scope classification layer.
// ---------------------------------------------------------------------

ManifestVerificationDiagnosticFinding qmlFinding(
    std::string relativePath, int line, int column, std::string tag, std::string text)
{
    return {"<candidate>/" + relativePath, line, column, std::move(tag), std::move(text)};
}

void testResolveQmlLintCriterionDeltaPassesCleanBaselineAndCandidate()
{
    const auto delta = resolveQmlLintCriterionDelta({}, true, {}, {});
    require(delta.passed && delta.entries.empty(),
        "Clean baseline and candidate must pass with nothing to report");
}

void testResolveQmlLintCriterionDeltaTreatsRetainedFindingAsPreExistingDebt()
{
    const auto finding = qmlFinding("qml/AppSidebar.qml", 4, 1, "unqualified", "Unqualified access");
    const auto delta = resolveQmlLintCriterionDelta({finding}, true, {finding}, {});
    require(delta.passed, "An unchanged retained finding must not fail the criterion");
    require(delta.entries.size() == 1
            && delta.entries.front().provenance
                == QmlLintFindingProvenance::PreExistingVerificationDebt,
        "The retained finding must classify as PreExistingVerificationDebt");
}

void testResolveQmlLintCriterionDeltaOmitsResolvedBaselineDebtFromEntries()
{
    const auto finding = qmlFinding("qml/AppSidebar.qml", 4, 1, "unqualified", "Unqualified access");
    const auto delta = resolveQmlLintCriterionDelta({finding}, true, {}, {});
    require(delta.passed, "A resolved (eliminated) baseline finding must pass");
    require(delta.entries.empty(),
        "An eliminated finding has nothing new to report and must not appear as an entry");
}

// QML-lint advisory-severity policy (2026-09-28): a Hard-severity tag (not
// the Advisory-only "unqualified" used elsewhere in this file) is the one
// that must still fail - see testResolveQmlLintCriterionDeltaPassesOnNewInScopeAdvisoryFinding
// for the deliberately-different Advisory carve-out this test does NOT cover.
void testResolveQmlLintCriterionDeltaFailsOnNewInScopeFinding()
{
    const auto finding = qmlFinding("qml/AppCard.qml", 2, 1, "import", "Failed to import module");
    const std::unordered_set<std::string> owned {detail::normalizedPathKey(fs::path("qml/AppCard.qml"))};
    const auto delta = resolveQmlLintCriterionDelta({}, true, {finding}, owned);
    require(!delta.passed, "A genuinely new, Manifest-owned, Hard finding must fail the criterion");
    require(delta.entries.size() == 1
            && delta.entries.front().provenance
                == QmlLintFindingProvenance::CandidateIntroducedInScope
            && delta.entries.front().severity == QmlLintFindingSeverity::Hard
            && delta.entries.front().relativePath == "qml/AppCard.qml",
        "The new finding must classify as CandidateIntroducedInScope/Hard with its safe relative path");
}

void testResolveQmlLintCriterionDeltaFailsOnNewOutsideManifestFinding()
{
    const auto finding = qmlFinding("qml/Unrelated.qml", 2, 1, "unqualified", "Unqualified access");
    const auto delta = resolveQmlLintCriterionDelta({}, true, {finding}, {});
    require(!delta.passed, "A new finding outside every task's ownership must fail the criterion");
    require(delta.entries.size() == 1
            && delta.entries.front().provenance
                == QmlLintFindingProvenance::CandidateIntroducedOutsideManifest
            && delta.entries.front().relativePath == "qml/Unrelated.qml",
        "The new finding must classify as CandidateIntroducedOutsideManifest with its safe path");
}

void testResolveQmlLintCriterionDeltaRelocatedFindingStaysDebt()
{
    // Same code/path/tag/text, moved line/column - fingerprintQmlLintFinding()
    // deliberately excludes line/column (see its own doc comment), so this
    // remains one unambiguous Retained match, never Eliminated+New.
    const auto before = qmlFinding("qml/AppSidebar.qml", 4, 1, "unqualified", "Unqualified access");
    const auto after = qmlFinding("qml/AppSidebar.qml", 9, 3, "unqualified", "Unqualified access");
    const auto delta = resolveQmlLintCriterionDelta({before}, true, {after}, {});
    require(delta.passed, "A relocated-but-otherwise-identical finding must remain debt");
    require(delta.entries.size() == 1
            && delta.entries.front().provenance
                == QmlLintFindingProvenance::PreExistingVerificationDebt,
        "The relocated finding must classify as PreExistingVerificationDebt, not a new finding");
}

void testResolveQmlLintCriterionDeltaExtraDuplicateInstanceIsNewFinding()
{
    const auto finding = qmlFinding("qml/AppSidebar.qml", 4, 1, "unqualified", "Unqualified access");
    // One instance in baseline, two identical instances in candidate: the
    // extra second instance must be counted as a genuine new finding, never
    // silently absorbed as if the multiset already contained it.
    const auto delta = resolveQmlLintCriterionDelta({finding}, true, {finding, finding}, {});
    require(!delta.passed, "An extra duplicate instance of an existing warning must be a new finding");
    const auto newCount = std::count_if(delta.entries.begin(), delta.entries.end(),
        [](const QmlLintCriterionDeltaEntry& entry) {
            return entry.provenance == QmlLintFindingProvenance::CandidateIntroducedOutsideManifest;
        });
    const auto debtCount = std::count_if(delta.entries.begin(), delta.entries.end(),
        [](const QmlLintCriterionDeltaEntry& entry) {
            return entry.provenance == QmlLintFindingProvenance::PreExistingVerificationDebt;
        });
    require(delta.entries.size() == 2 && newCount == 1 && debtCount == 1,
        "Exactly one entry (the extra instance) must be reported as new; the matched instance is debt");
}

void testResolveQmlLintCriterionDeltaUnmappablePathIsUnattributable()
{
    // No "<candidate>/" prefix at all - displayQmlLintFindingPath() cannot
    // safely resolve a candidate-relative path from this fileKey.
    const ManifestVerificationDiagnosticFinding finding {
        "diagnostic source unavailable", 0, 0, "unqualified", "Unqualified access"};
    const auto delta = resolveQmlLintCriterionDelta({}, true, {finding}, {});
    require(!delta.passed, "An unmappable new finding must fail the criterion");
    require(delta.entries.size() == 1
            && delta.entries.front().provenance == QmlLintFindingProvenance::Unattributable
            && delta.entries.front().relativePath.empty(),
        "The unmappable finding must classify as Unattributable with no guessed path");
}

void testResolveQmlLintCriterionDeltaFailsClosedWhenBaselineEvidenceUnavailable()
{
    const auto finding = qmlFinding("qml/AppSidebar.qml", 4, 1, "unqualified", "Unqualified access");
    // Even though this exact finding also appears in the (ignored) baseline
    // list, baselineEvidenceAvailable=false means it can never be proven
    // pre-existing - a broken baseline probe must never launder a real
    // regression as old debt.
    const auto delta = resolveQmlLintCriterionDelta({finding}, false, {finding}, {});
    require(!delta.passed, "Missing baseline evidence must fail closed rather than assume debt");
    require(delta.entries.size() == 1
            && delta.entries.front().provenance == QmlLintFindingProvenance::Unattributable,
        "Every candidate finding must classify as Unattributable when baseline evidence is missing");
}

void testResolveQmlLintCriterionDeltaFailsClosedOnRetainedUnsafePath()
{
    const ManifestVerificationDiagnosticFinding finding {
        "<candidate>/qml/../outside.qml", 2, 1, "unqualified", "Unqualified access"};
    const auto delta = resolveQmlLintCriterionDelta({finding}, true, {finding}, {});
    require(!delta.passed,
        "A retained finding with an unsafe path must not be laundered as baseline debt");
    require(delta.entries.size() == 1
            && delta.entries.front().provenance == QmlLintFindingProvenance::Unattributable,
        "The retained unsafe finding must remain fail-closed and unattributable");
}

// ---------------------------------------------------------------------
// QML-lint advisory-severity policy (2026-09-28, real Stage 4A Run
// b4123f17-fb4f-4977-9a22-28e8d3d15e23): resolveQmlLintCriterionDelta() must
// let a genuinely new, Manifest-owned, Advisory-severity finding pass - and
// only that exact combination. Every other shape (outside Manifest,
// unattributable, missing baseline evidence, mixed with a Hard finding)
// stays fail-closed exactly as before, regardless of any Advisory tag
// present.
// ---------------------------------------------------------------------

void testResolveQmlLintCriterionDeltaPassesOnNewInScopeAdvisoryFinding()
{
    const auto finding = qmlFinding("qml/AlertsView.qml", 2, 1, "unqualified", "Unqualified access");
    const std::unordered_set<std::string> owned {
        detail::normalizedPathKey(fs::path("qml/AlertsView.qml"))};
    const auto delta = resolveQmlLintCriterionDelta({}, true, {finding}, owned);
    require(delta.passed,
        "A new, Manifest-owned, Advisory-severity finding must now pass the criterion");
    require(delta.entries.size() == 1
            && delta.entries.front().provenance == QmlLintFindingProvenance::CandidateIntroducedInScope
            && delta.entries.front().severity == QmlLintFindingSeverity::Advisory
            && delta.entries.front().relativePath == "qml/AlertsView.qml",
        "The finding must still be reported as CandidateIntroducedInScope/Advisory, never dropped");
}

void testResolveQmlLintCriterionDeltaRecognizesUnusedImportsAsAdvisoryInScope()
{
    const auto finding = qmlFinding("qml/AlertsView.qml", 1, 1, "unused-imports", "Unused import");
    const std::unordered_set<std::string> owned {
        detail::normalizedPathKey(fs::path("qml/AlertsView.qml"))};
    const auto delta = resolveQmlLintCriterionDelta({}, true, {finding}, owned);
    require(delta.passed, "A new, in-scope unused-imports finding must also pass");
    require(delta.entries.size() == 1
            && delta.entries.front().severity == QmlLintFindingSeverity::Advisory,
        "unused-imports must classify Advisory through the same resolver path");
}

void testResolveQmlLintCriterionDeltaStillFailsOnNewOutsideManifestAdvisoryFinding()
{
    const auto finding = qmlFinding("qml/Unowned.qml", 2, 1, "unqualified", "Unqualified access");
    const auto delta = resolveQmlLintCriterionDelta({}, true, {finding}, {});
    require(!delta.passed,
        "An Advisory-tagged finding outside every task's ownership must still fail closed");
    require(delta.entries.size() == 1
            && delta.entries.front().provenance
                == QmlLintFindingProvenance::CandidateIntroducedOutsideManifest,
        "Outside-Manifest provenance must never be overridden by an Advisory tag");
}

void testResolveQmlLintCriterionDeltaStillFailsOnUnattributableAdvisoryFinding()
{
    const ManifestVerificationDiagnosticFinding finding {
        "diagnostic source unavailable", 0, 0, "unqualified", "Unqualified access"};
    const auto delta = resolveQmlLintCriterionDelta({}, true, {finding}, {});
    require(!delta.passed, "Unattributable evidence must fail closed even with an Advisory tag");
    require(delta.entries.size() == 1
            && delta.entries.front().provenance == QmlLintFindingProvenance::Unattributable,
        "An unmappable finding must never be laundered into a safe Advisory pass");
}

void testResolveQmlLintCriterionDeltaStillFailsWhenBaselineUnavailableForAdvisoryFinding()
{
    const auto finding = qmlFinding("qml/AlertsView.qml", 2, 1, "unqualified", "Unqualified access");
    const std::unordered_set<std::string> owned {
        detail::normalizedPathKey(fs::path("qml/AlertsView.qml"))};
    const auto delta = resolveQmlLintCriterionDelta({finding}, false, {finding}, owned);
    require(!delta.passed,
        "Missing baseline evidence must fail closed regardless of the finding's own severity");
    require(delta.entries.size() == 1
            && delta.entries.front().provenance == QmlLintFindingProvenance::Unattributable,
        "Without baseline evidence, even a would-be Advisory finding classifies Unattributable");
}

void testResolveQmlLintCriterionDeltaMixedAdvisoryAndHardInScopeFindingsFails()
{
    const auto advisory = qmlFinding("qml/AlertsView.qml", 2, 1, "unqualified", "Unqualified access");
    const auto hard = qmlFinding("qml/MachinesView.qml", 4, 1, "import", "Failed to import module");
    const std::unordered_set<std::string> owned {
        detail::normalizedPathKey(fs::path("qml/AlertsView.qml")),
        detail::normalizedPathKey(fs::path("qml/MachinesView.qml"))};
    const auto delta = resolveQmlLintCriterionDelta({}, true, {advisory, hard}, owned);
    require(!delta.passed,
        "A single new Hard in-scope finding must fail the criterion even alongside an Advisory one");
    require(delta.entries.size() == 2, "Both findings must still be reported");
    const auto hardEntry = std::find_if(delta.entries.begin(), delta.entries.end(),
        [](const QmlLintCriterionDeltaEntry& entry) {
            return entry.severity == QmlLintFindingSeverity::Hard;
        });
    const auto advisoryEntry = std::find_if(delta.entries.begin(), delta.entries.end(),
        [](const QmlLintCriterionDeltaEntry& entry) {
            return entry.severity == QmlLintFindingSeverity::Advisory;
        });
    require(hardEntry != delta.entries.end() && advisoryEntry != delta.entries.end()
            && hardEntry->provenance == QmlLintFindingProvenance::CandidateIntroducedInScope
            && advisoryEntry->provenance == QmlLintFindingProvenance::CandidateIntroducedInScope,
        "Each finding must keep its own independent severity/provenance classification");
}

// A genuine duplicate-instance regression (real Stage 4A shape: the same
// Advisory tag/message recurring across several files/lines) must still be
// counted per-instance by the shared multiset resolver, never collapsed.
void testResolveQmlLintCriterionDeltaDuplicateAdvisoryInstancesAllCountedAsNew()
{
    const auto a = qmlFinding("qml/AlertsView.qml", 2, 1, "unqualified", "Unqualified access");
    const auto b = qmlFinding("qml/MachinesView.qml", 4, 1, "unqualified", "Unqualified access");
    const std::unordered_set<std::string> owned {
        detail::normalizedPathKey(fs::path("qml/AlertsView.qml")),
        detail::normalizedPathKey(fs::path("qml/MachinesView.qml"))};
    const auto delta = resolveQmlLintCriterionDelta({}, true, {a, a, b}, owned);
    require(delta.passed, "Multiple distinct new Advisory findings must still pass together");
    require(delta.entries.size() == 3,
        "Every one of the three new Advisory instances must be reported, none silently merged");
    require(std::all_of(delta.entries.begin(), delta.entries.end(),
            [](const QmlLintCriterionDeltaEntry& entry) {
                return entry.severity == QmlLintFindingSeverity::Advisory
                    && entry.provenance == QmlLintFindingProvenance::CandidateIntroducedInScope;
            }),
        "Every duplicate instance must independently classify Advisory/CandidateIntroducedInScope");
}

// Stage 7B Run 1c37cc37: the same message under the same [unqualified] tag must NOT take the advisory
// carve-out, even for a new, Manifest-owned finding - it fails the criterion; the harmless finding
// next to it keeps its advisory shape; and the identical finding already present in the baseline is
// still plain pre-existing debt (the policy never blames a candidate for a repository's own state).
void testResolveQmlLintCriterionDeltaFailsOnNewMissingSignalHandlerUnderAdvisoryTag()
{
    const auto handler = qmlFinding("qml/components/dashboard/RecentAlertsPanel.qml", 110, 9,
        "unqualified", "no matching signal found for handler \"onMaxItemsChanged\" [unqualified]");
    const auto advisory = qmlFinding("qml/components/dashboard/RecentAlertsPanel.qml", 12, 3,
        "unqualified", "Unqualified access [unqualified]");
    const std::unordered_set<std::string> owned {
        detail::normalizedPathKey(fs::path("qml/components/dashboard/RecentAlertsPanel.qml"))};

    const auto failing = resolveQmlLintCriterionDelta({}, true, {handler}, owned);
    require(!failing.passed,
        "A new in-scope handler-without-a-signal finding must fail the criterion");
    require(failing.entries.size() == 1
            && failing.entries.front().severity == QmlLintFindingSeverity::Hard
            && failing.entries.front().provenance
                == QmlLintFindingProvenance::CandidateIntroducedInScope,
        "It must be reported as a Hard, candidate-introduced in-scope finding");

    const auto mixed = resolveQmlLintCriterionDelta({}, true, {advisory, handler}, owned);
    require(!mixed.passed && mixed.entries.size() == 2,
        "An advisory finding next to it must not rescue the criterion");
    const auto advisoryEntry = std::find_if(mixed.entries.begin(), mixed.entries.end(),
        [](const QmlLintCriterionDeltaEntry& entry) {
            return entry.severity == QmlLintFindingSeverity::Advisory;
        });
    require(advisoryEntry != mixed.entries.end()
            && advisoryEntry->finding.text.find("Unqualified access") != std::string::npos,
        "The harmless finding keeps its own Advisory classification");

    const auto debt = resolveQmlLintCriterionDelta({handler}, true, {handler}, owned);
    require(debt.passed && debt.entries.size() == 1
            && debt.entries.front().provenance == QmlLintFindingProvenance::PreExistingVerificationDebt,
        "The same finding already in the baseline is unchanged pre-existing debt, not the "
        "candidate's");
}

// ---------------------------------------------------------------------
// Post-Stage-3 QML-lint hardening: real ManifestProposalSetVerifier::verify()
// integration for a project-wide/integration qml_lint requirement (the
// owning task has no owned or Backend-derived-dependency QML path - the
// exact historical fail-closed trigger). Each test supplies a real baseline
// overlay (built the same way materialize()/prepare() already do, via
// overlayRequest(fixture, {}) - an empty proposal-id overlay over the same
// captured baseline) so the shared verifier can run the identical lint
// command against both roots.
// ---------------------------------------------------------------------

ManifestProposalOverlayDescriptor buildBaselineOverlay(Fixture& fixture)
{
    const auto overlay = ManifestProposalOverlay::build(overlayRequest(fixture, {}), fixture.service);
    require(overlay.success(), "Fixture baseline-only overlay failed to build");
    return overlay.value();
}

ExecutionManifestTask integrationTaskWithoutQmlScope()
{
    ExecutionAcceptanceRequirement lint;
    lint.id = "lint";
    lint.description = "Project-wide QML lint";
    lint.verificationKind = ExecutionVerificationKind::QmlLint;
    lint.verificationArguments = {{"buildDirectory", "build"}, {"target", "all_qmllint"}};
    lint.qmlLintDiagnosticPolicyId =
        std::string(kQmlLintDiagnosticPolicyAdvisoryV1);
    auto task = taskWithRequirement(lint);
    task.id = "integration";
    task.ownedPaths = {"qml/CMakeLists.txt"};
    return task;
}

void testProjectWideQmlLintPassesWhenBaselineAndCandidateBothClean()
{
    Fixture fixture;
    fixture.add("p1", {change("a", FileChangeOperation::Create, "qml/CMakeLists.txt", "# cmake")});
    const auto candidateOverlay = ManifestProposalOverlay::build(
        overlayRequest(fixture, {"p1"}), fixture.service);
    require(candidateOverlay.success(), "Fixture candidate overlay failed to build");
    const auto baselineOverlay = buildBaselineOverlay(fixture);

    ManifestProposalSetVerifierRequest request {
        candidateOverlay.value(), {integrationTaskWithoutQmlScope()}, &baselineOverlay};
    FakeCommandRunner runner;
    runner.outcomes.push_back(successfulProbe("Lint passed with no warnings.\n"));
    runner.outcomes.push_back(successfulProbe("Lint passed with no warnings.\n"));

    const auto verification = ManifestProposalSetVerifier::verify(request, fixture.service, runner, {});
    require(verification.success(), "Verification call itself failed");
    require(verification.value().checks.front().status == ExecutionVerificationStatus::Passed,
        "A genuinely clean baseline and candidate must pass a project-wide QML lint criterion");
    require(runner.calls == 1,
        "A clean candidate needs no baseline probe at all - it already has nothing to reclassify");
}

void testProjectWideQmlLintPassesOnUnchangedBaselineDebt()
{
    Fixture fixture;
    fixture.add("p1", {change("a", FileChangeOperation::Create, "qml/CMakeLists.txt", "# cmake")});
    const auto candidateOverlay = ManifestProposalOverlay::build(
        overlayRequest(fixture, {"p1"}), fixture.service);
    require(candidateOverlay.success(), "Fixture candidate overlay failed to build");
    const auto baselineOverlay = buildBaselineOverlay(fixture);

    ManifestProposalSetVerifierRequest request {
        candidateOverlay.value(), {integrationTaskWithoutQmlScope()}, &baselineOverlay};
    FakeCommandRunner runner;
    const auto warning = successfulProbe(
        "Warning: <candidate>/qml/AppShell.qml:1:1: Unqualified access [unqualified]\n");
    runner.outcomes.push_back(warning); // candidate
    runner.outcomes.push_back(warning); // baseline: byte-identical warning

    const auto verification = ManifestProposalSetVerifier::verify(request, fixture.service, runner, {});
    require(verification.success(), "Verification call itself failed");
    const auto& check = verification.value().checks.front();
    require(check.status == ExecutionVerificationStatus::Passed,
        "A project-wide task must not fail on unchanged pre-existing project debt");
    const bool hasDebtDiagnostic = std::any_of(check.diagnostics.begin(), check.diagnostics.end(),
        [](const ManifestVerificationDiagnostic& diagnostic) {
            return diagnostic.code == "verification.qml_warning_baseline_debt";
        });
    require(hasDebtDiagnostic,
        "The unchanged debt must still be honestly projected as a non-blocking diagnostic");
}

// Regression for Runs 030ce232 / 9a344ee3 (2026-10): the baseline had more
// warnings than the 12-line excerpt cap (20 here: 10 files x 2), so its
// structured findings were a per-file SAMPLE, while a candidate that fixed
// most of them (8 left) fit entirely under the cap. Comparing the two samples
// reported the candidate's retained warnings as "new outside the Manifest".
// The delta must be decided on the complete sets.
std::string unqualifiedWarnings(int firstFile, int lastFile, int lineOffset)
{
    std::string output;
    for (int file = firstFile; file <= lastFile; ++file) {
        for (int row = 0; row < 2; ++row) {
            output += "Warning: <candidate>/qml/File" + std::to_string(file) + ".qml:"
                + std::to_string(lineOffset + row * 5) + ":3: Unqualified access [unqualified]\n";
        }
    }
    return output;
}

void testProjectWideQmlLintComparesCompleteSetsNotTruncatedSamples()
{
    Fixture fixture;
    fixture.add("p1", {change("a", FileChangeOperation::Create, "qml/CMakeLists.txt", "# cmake")});
    const auto candidateOverlay = ManifestProposalOverlay::build(
        overlayRequest(fixture, {"p1"}), fixture.service);
    require(candidateOverlay.success(), "Fixture candidate overlay failed to build");
    const auto baselineOverlay = buildBaselineOverlay(fixture);

    ManifestProposalSetVerifierRequest request {
        candidateOverlay.value(), {integrationTaskWithoutQmlScope()}, &baselineOverlay};
    FakeCommandRunner runner;
    // Candidate: files 5..8 keep both of their warnings (8 lines, shifted).
    runner.outcomes.push_back(successfulProbe(unqualifiedWarnings(5, 8, 20)));
    // Baseline: files 1..10, two warnings each (20 lines > the 12-line cap).
    runner.outcomes.push_back(successfulProbe(unqualifiedWarnings(1, 10, 10)));

    const auto verification = ManifestProposalSetVerifier::verify(request, fixture.service, runner, {});
    require(verification.success(), "Verification call itself failed");
    const auto& check = verification.value().checks.front();
    require(check.status == ExecutionVerificationStatus::Passed,
        "Warnings that all exist in the baseline must pass even when the baseline output was "
        "larger than the excerpt cap");
    for (const auto& diagnostic : check.diagnostics) {
        require(diagnostic.code != "verification.qml_warning_outside_manifest"
                && diagnostic.code != "verification.qml_warning",
            "A truncated baseline sample must never make a retained warning look new");
    }
    const auto debt = std::find_if(check.diagnostics.begin(), check.diagnostics.end(),
        [](const ManifestVerificationDiagnostic& diagnostic) {
            return diagnostic.code == "verification.qml_warning_baseline_debt";
        });
    require(debt != check.diagnostics.end() && debt->structuredFindings.size() == 8,
        "All eight retained warnings must be reported as baseline debt");
}

// The same incident through the precomputed-baseline path (continuation and
// manual-handoff reverify): the source-only baseline verification must carry
// complete findings, not an excerpt sample.
void testPrecomputedBaselineVerificationCarriesCompleteQmlFindings()
{
    Fixture fixture;
    fixture.add("p1", {change("a", FileChangeOperation::Create, "qml/CMakeLists.txt", "# cmake")});
    const auto candidateOverlay = ManifestProposalOverlay::build(
        overlayRequest(fixture, {"p1"}), fixture.service);
    require(candidateOverlay.success(), "Fixture candidate overlay failed to build");
    const auto baselineOverlay = buildBaselineOverlay(fixture);

    FakeCommandRunner baselineRunner;
    baselineRunner.outcomes.push_back(successfulProbe(unqualifiedWarnings(1, 10, 10)));
    ManifestProposalSetVerifierRequest baselineRequest {
        baselineOverlay, {integrationTaskWithoutQmlScope()}};
    const auto baselineVerification = ManifestProposalSetVerifier::verify(
        baselineRequest, fixture.service, baselineRunner, {});
    require(baselineVerification.success(), "Baseline verification call itself failed");
    std::size_t baselineFindingCount = 0;
    for (const auto& diagnostic : baselineVerification.value().diagnostics) {
        if (diagnostic.code == "verification.qml_warning") {
            baselineFindingCount += diagnostic.structuredFindings.size();
        }
    }
    require(baselineFindingCount == 20,
        "A source-only baseline verification must keep every finding, not a 12-line sample");

    ManifestProposalSetVerifierRequest candidateRequest {
        candidateOverlay.value(), {integrationTaskWithoutQmlScope()}, &baselineOverlay,
        &baselineVerification.value()};
    FakeCommandRunner candidateRunner;
    candidateRunner.outcomes.push_back(successfulProbe(unqualifiedWarnings(5, 8, 20)));
    const auto verification = ManifestProposalSetVerifier::verify(
        candidateRequest, fixture.service, candidateRunner, {});
    require(verification.success(), "Candidate verification call itself failed");
    require(verification.value().checks.front().status == ExecutionVerificationStatus::Passed,
        "Warnings present in the precomputed baseline must pass the delta");
    require(candidateRunner.calls == 1,
        "A precomputed baseline must not re-run the baseline lint command");
}

// More findings than can be compared exactly must fail closed with a distinct
// diagnostic, never fall back to comparing samples.
void testQmlLintFailsClosedWhenFindingsExceedComparableBound()
{
    Fixture fixture;
    fixture.add("p1", {change("a", FileChangeOperation::Create, "qml/CMakeLists.txt", "# cmake")});
    const auto candidateOverlay = ManifestProposalOverlay::build(
        overlayRequest(fixture, {"p1"}), fixture.service);
    require(candidateOverlay.success(), "Fixture candidate overlay failed to build");
    const auto baselineOverlay = buildBaselineOverlay(fixture);

    ManifestProposalSetVerifierRequest request {
        candidateOverlay.value(), {integrationTaskWithoutQmlScope()}, &baselineOverlay};
    FakeCommandRunner runner;
    std::string flood;
    for (int index = 0; index < 1100; ++index) {
        flood += "Warning: <candidate>/qml/Flood" + std::to_string(index)
            + ".qml:1:1: Unqualified access [unqualified]\n";
    }
    runner.outcomes.push_back(successfulProbe(flood));
    runner.outcomes.push_back(successfulProbe(flood));

    const auto verification = ManifestProposalSetVerifier::verify(request, fixture.service, runner, {});
    require(verification.success(), "Verification call itself failed");
    const auto& check = verification.value().checks.front();
    require(check.status == ExecutionVerificationStatus::Failed,
        "An uncomparable amount of lint output must not pass");
    require(std::any_of(check.diagnostics.begin(), check.diagnostics.end(),
                [](const ManifestVerificationDiagnostic& diagnostic) {
                    return diagnostic.code == "verification.qml_findings_overflow";
                }),
        "The overflow must be reported with its own stable diagnostic code");
}

void testProjectWideQmlLintPassesWhenBaselineWarningResolved()
{
    Fixture fixture;
    fixture.add("p1", {change("a", FileChangeOperation::Create, "qml/CMakeLists.txt", "# cmake")});
    const auto candidateOverlay = ManifestProposalOverlay::build(
        overlayRequest(fixture, {"p1"}), fixture.service);
    require(candidateOverlay.success(), "Fixture candidate overlay failed to build");
    const auto baselineOverlay = buildBaselineOverlay(fixture);

    ManifestProposalSetVerifierRequest request {
        candidateOverlay.value(), {integrationTaskWithoutQmlScope()}, &baselineOverlay};
    FakeCommandRunner runner;
    runner.outcomes.push_back(successfulProbe("Lint passed with no warnings.\n")); // candidate: clean
    runner.outcomes.push_back(successfulProbe(
        "Warning: <candidate>/qml/AppShell.qml:1:1: Unqualified access [unqualified]\n")); // baseline had it

    const auto verification = ManifestProposalSetVerifier::verify(request, fixture.service, runner, {});
    require(verification.success(), "Verification call itself failed");
    require(verification.value().checks.front().status == ExecutionVerificationStatus::Passed,
        "A resolved baseline warning must not itself block a clean candidate");
}

// QML-lint advisory-severity policy (2026-09-28): uses a Hard tag
// ("import") deliberately, not "unqualified" - see
// testProjectWideQmlLintPassesOnAdvisoryOnlyNewWarning for the now-different
// Advisory outcome on the same shape of fixture.
void testProjectWideQmlLintFailsOnNewInScopeWarning()
{
    Fixture fixture;
    fixture.add("p1", {
        change("a", FileChangeOperation::Create, "qml/CMakeLists.txt", "# cmake"),
        change("b", FileChangeOperation::Create, "qml/AppCard.qml", "Item {}")});
    const auto candidateOverlay = ManifestProposalOverlay::build(
        overlayRequest(fixture, {"p1"}), fixture.service);
    require(candidateOverlay.success(), "Fixture candidate overlay failed to build");
    const auto baselineOverlay = buildBaselineOverlay(fixture);

    ExecutionManifestTask owner;
    owner.id = "component";
    owner.objective = "Owns AppCard.qml";
    owner.role = "component-author";
    owner.requestedCapability = WorkspaceCapability::Propose;
    owner.ownedPaths = {"qml/AppCard.qml"};
    ManifestProposalSetVerifierRequest request {
        candidateOverlay.value(), {integrationTaskWithoutQmlScope(), owner}, &baselineOverlay};
    FakeCommandRunner runner;
    runner.outcomes.push_back(successfulProbe(
        "Warning: <candidate>/qml/AppCard.qml:2:1: Failed to import module [import]\n")); // candidate: new, Hard
    runner.outcomes.push_back(successfulProbe("Lint passed with no warnings.\n")); // baseline: clean

    const auto verification = ManifestProposalSetVerifier::verify(request, fixture.service, runner, {});
    require(verification.success(), "Verification call itself failed");
    const auto& check = verification.value().checks.front();
    require(check.status == ExecutionVerificationStatus::Failed,
        "A genuinely new warning on a Manifest-owned path must fail a project-wide criterion");
    const bool hasBlockingDiagnostic = std::any_of(check.diagnostics.begin(), check.diagnostics.end(),
        [](const ManifestVerificationDiagnostic& diagnostic) {
            return diagnostic.code == "verification.qml_warning";
        });
    require(hasBlockingDiagnostic, "The new in-scope finding must surface as the ordinary blocking code");
}

void testProjectWideQmlLintFailsOnNewOutsideManifestWarning()
{
    Fixture fixture;
    fixture.add("p1", {change("a", FileChangeOperation::Create, "qml/CMakeLists.txt", "# cmake")});
    const auto candidateOverlay = ManifestProposalOverlay::build(
        overlayRequest(fixture, {"p1"}), fixture.service);
    require(candidateOverlay.success(), "Fixture candidate overlay failed to build");
    const auto baselineOverlay = buildBaselineOverlay(fixture);

    ManifestProposalSetVerifierRequest request {
        candidateOverlay.value(), {integrationTaskWithoutQmlScope()}, &baselineOverlay};
    FakeCommandRunner runner;
    runner.outcomes.push_back(successfulProbe(
        "Warning: <candidate>/qml/Unowned.qml:2:1: Unqualified access [unqualified]\n"));
    runner.outcomes.push_back(successfulProbe("Lint passed with no warnings.\n"));

    const auto verification = ManifestProposalSetVerifier::verify(request, fixture.service, runner, {});
    require(verification.success(), "Verification call itself failed");
    const auto& check = verification.value().checks.front();
    require(check.status == ExecutionVerificationStatus::Failed,
        "A new warning on a path no task owns must fail closed, never silently pass");
    const bool hasOutsideManifestDiagnostic = std::any_of(
        check.diagnostics.begin(), check.diagnostics.end(),
        [](const ManifestVerificationDiagnostic& diagnostic) {
            return diagnostic.code == "verification.qml_warning_outside_manifest"
                && diagnostic.structuredFindings.size() == 1
                && diagnostic.structuredFindings.front().fileKey.find("qml/Unowned.qml")
                    != std::string::npos;
        });
    require(hasOutsideManifestDiagnostic,
        "The new outside-Manifest finding must name its exact safe relative path");
}

void testProjectWideQmlLintMixedDebtAndNewWarningFailsOnlyForNew()
{
    Fixture fixture;
    fixture.add("p1", {change("a", FileChangeOperation::Create, "qml/CMakeLists.txt", "# cmake")});
    const auto candidateOverlay = ManifestProposalOverlay::build(
        overlayRequest(fixture, {"p1"}), fixture.service);
    require(candidateOverlay.success(), "Fixture candidate overlay failed to build");
    const auto baselineOverlay = buildBaselineOverlay(fixture);

    ManifestProposalSetVerifierRequest request {
        candidateOverlay.value(), {integrationTaskWithoutQmlScope()}, &baselineOverlay};
    FakeCommandRunner runner;
    runner.outcomes.push_back(successfulProbe(
        "Warning: <candidate>/qml/AppShell.qml:1:1: Unqualified access [unqualified]\n"
        "Warning: <candidate>/qml/NewFile.qml:5:2: Unqualified access [unqualified]\n"));
    runner.outcomes.push_back(successfulProbe(
        "Warning: <candidate>/qml/AppShell.qml:1:1: Unqualified access [unqualified]\n"));

    const auto verification = ManifestProposalSetVerifier::verify(request, fixture.service, runner, {});
    require(verification.success(), "Verification call itself failed");
    const auto& check = verification.value().checks.front();
    require(check.status == ExecutionVerificationStatus::Failed,
        "A mixed set must fail because of the new finding, regardless of the retained debt");
    bool sawDebt = false;
    bool sawNew = false;
    for (const auto& diagnostic : check.diagnostics) {
        if (diagnostic.code == "verification.qml_warning_baseline_debt") sawDebt = true;
        if (diagnostic.code == "verification.qml_warning_outside_manifest") sawNew = true;
    }
    require(sawDebt && sawNew,
        "Both the unchanged debt and the new finding must be honestly, separately projected");
}

void testProjectWideQmlLintFailsClosedWhenBaselineProbeToolUnavailable()
{
    Fixture fixture;
    fixture.add("p1", {change("a", FileChangeOperation::Create, "qml/CMakeLists.txt", "# cmake")});
    const auto candidateOverlay = ManifestProposalOverlay::build(
        overlayRequest(fixture, {"p1"}), fixture.service);
    require(candidateOverlay.success(), "Fixture candidate overlay failed to build");
    const auto baselineOverlay = buildBaselineOverlay(fixture);

    ManifestProposalSetVerifierRequest request {
        candidateOverlay.value(), {integrationTaskWithoutQmlScope()}, &baselineOverlay};
    FakeCommandRunner runner;
    runner.outcomes.push_back(successfulProbe(
        "Warning: <candidate>/qml/AppShell.qml:1:1: Unqualified access [unqualified]\n"));
    ManifestVerificationCommandOutcome toolFailure;
    toolFailure.started = false;
    toolFailure.errorCode = "verification.tool_unavailable";
    runner.outcomes.push_back(toolFailure); // baseline probe itself never started

    const auto verification = ManifestProposalSetVerifier::verify(request, fixture.service, runner, {});
    require(verification.success(), "Verification call itself failed");
    require(verification.value().checks.front().status == ExecutionVerificationStatus::Failed,
        "A broken baseline probe must never let a real candidate warning pass as unproven debt");
    const auto& diagnostics = verification.value().checks.front().diagnostics;
    require(std::any_of(diagnostics.begin(), diagnostics.end(),
            [](const ManifestVerificationDiagnostic& diagnostic) {
                return diagnostic.code == "verification.qml_baseline_unavailable";
            }),
        "A broken baseline probe must surface a stable non-repairable evidence code");
}

// ---------------------------------------------------------------------
// QML-lint advisory-severity policy (2026-09-28, real Stage 4A Run
// b4123f17-fb4f-4977-9a22-28e8d3d15e23): ManifestProposalSetVerifier::verify()
// integration for qmlLintBaselineDeltaCheck()'s new advisory bucket/diagnostic.
// ---------------------------------------------------------------------

void testProjectWideQmlLintPassesOnAdvisoryOnlyNewWarning()
{
    Fixture fixture;
    fixture.add("p1", {
        change("a", FileChangeOperation::Create, "qml/CMakeLists.txt", "# cmake"),
        change("b", FileChangeOperation::Create, "qml/AlertsView.qml", "Item {}")});
    const auto candidateOverlay = ManifestProposalOverlay::build(
        overlayRequest(fixture, {"p1"}), fixture.service);
    require(candidateOverlay.success(), "Fixture candidate overlay failed to build");
    const auto baselineOverlay = buildBaselineOverlay(fixture);

    ExecutionManifestTask owner;
    owner.id = "component";
    owner.objective = "Owns AlertsView.qml";
    owner.role = "component-author";
    owner.requestedCapability = WorkspaceCapability::Propose;
    owner.ownedPaths = {"qml/AlertsView.qml"};
    ManifestProposalSetVerifierRequest request {
        candidateOverlay.value(), {integrationTaskWithoutQmlScope(), owner}, &baselineOverlay};
    FakeCommandRunner runner;
    runner.outcomes.push_back(successfulProbe(
        "Warning: <candidate>/qml/AlertsView.qml:2:1: Unqualified access [unqualified]\n"));
    runner.outcomes.push_back(successfulProbe("Lint passed with no warnings.\n"));

    const auto verification = ManifestProposalSetVerifier::verify(request, fixture.service, runner, {});
    require(verification.success(), "Verification call itself failed");
    const auto& check = verification.value().checks.front();
    require(check.status == ExecutionVerificationStatus::Passed,
        "A genuinely new but Advisory-only in-scope finding must now PASS the criterion");
    require(requiredAcceptancePassed(verification.value()),
        "Advisory-only must flow through to the authoritative aggregate as Passed");
    bool sawAdvisory = false;
    bool sawBlocking = false;
    for (const auto& diagnostic : check.diagnostics) {
        if (diagnostic.code == "verification.qml_warning_advisory") {
            sawAdvisory = true;
            require(diagnostic.structuredFindings.size() == 1
                    && diagnostic.structuredFindings.front().fileKey.find("AlertsView.qml")
                        != std::string::npos,
                "The advisory diagnostic must carry the exact finding, never dropped");
        }
        if (diagnostic.code == "verification.qml_warning") sawBlocking = true;
    }
    require(sawAdvisory && !sawBlocking,
        "A Passed advisory-only check must carry verification.qml_warning_advisory and never "
        "the blocking verification.qml_warning code");
}

// The end-to-end shape of Stage 7B Run 1c37cc37: qmllint reports a handler for a signal the object
// does not have under [unqualified]; the frozen advisory policy must still FAIL the check and carry
// the finding in the blocking verification.qml_warning diagnostic, never the advisory one.
void testProjectWideQmlLintFailsOnNewMissingSignalHandlerDespiteAdvisoryTag()
{
    Fixture fixture;
    fixture.add("p1", {
        change("a", FileChangeOperation::Create, "qml/CMakeLists.txt", "# cmake"),
        change("b", FileChangeOperation::Create, "qml/AlertsView.qml", "Item {}")});
    const auto candidateOverlay = ManifestProposalOverlay::build(
        overlayRequest(fixture, {"p1"}), fixture.service);
    require(candidateOverlay.success(), "Fixture candidate overlay failed to build");
    const auto baselineOverlay = buildBaselineOverlay(fixture);

    ExecutionManifestTask owner;
    owner.id = "component";
    owner.objective = "Owns AlertsView.qml";
    owner.role = "component-author";
    owner.requestedCapability = WorkspaceCapability::Propose;
    owner.ownedPaths = {"qml/AlertsView.qml"};
    ManifestProposalSetVerifierRequest request {
        candidateOverlay.value(), {integrationTaskWithoutQmlScope(), owner}, &baselineOverlay};
    FakeCommandRunner runner;
    runner.outcomes.push_back(successfulProbe(
        "Warning: <candidate>/qml/AlertsView.qml:110:9: no matching signal found for handler "
        "\"onMaxItemsChanged\" [unqualified]\n"));
    runner.outcomes.push_back(successfulProbe("Lint passed with no warnings.\n"));

    const auto verification = ManifestProposalSetVerifier::verify(request, fixture.service, runner, {});
    require(verification.success(), "Verification call itself failed");
    const auto& check = verification.value().checks.front();
    require(check.status == ExecutionVerificationStatus::Failed,
        "A handler for a signal the object does not have must FAIL the qml_lint check even under "
        "the frozen advisory policy");
    require(!requiredAcceptancePassed(verification.value()),
        "The failure must flow to the authoritative aggregate");
    bool sawBlocking = false;
    for (const auto& diagnostic : check.diagnostics) {
        require(diagnostic.code != "verification.qml_warning_advisory",
            "The finding must never be filed as advisory");
        if (diagnostic.code == "verification.qml_warning") {
            sawBlocking = true;
            require(diagnostic.structuredFindings.size() == 1
                    && diagnostic.structuredFindings.front().text.find("onMaxItemsChanged")
                        != std::string::npos,
                "The blocking diagnostic must carry the exact finding");
        }
    }
    require(sawBlocking, "The finding must be reported under the blocking verification.qml_warning");
}

void testLegacyQmlLintRequirementWithoutFrozenPolicyKeepsAdvisoryTagBlocking()
{
    Fixture fixture;
    fixture.add("p1", {
        change("a", FileChangeOperation::Create, "qml/CMakeLists.txt", "# cmake"),
        change("b", FileChangeOperation::Create, "qml/AlertsView.qml", "Item {}")});
    const auto candidateOverlay = ManifestProposalOverlay::build(
        overlayRequest(fixture, {"p1"}), fixture.service);
    require(candidateOverlay.success(), "Fixture candidate overlay failed to build");
    const auto baselineOverlay = buildBaselineOverlay(fixture);

    auto integration = integrationTaskWithoutQmlScope();
    integration.acceptanceContract->requirements.front().qmlLintDiagnosticPolicyId.reset();
    ExecutionManifestTask owner;
    owner.id = "component";
    owner.objective = "Owns AlertsView.qml";
    owner.role = "component-author";
    owner.requestedCapability = WorkspaceCapability::Propose;
    owner.ownedPaths = {"qml/AlertsView.qml"};
    ManifestProposalSetVerifierRequest request {
        candidateOverlay.value(), {integration, owner}, &baselineOverlay};
    FakeCommandRunner runner;
    runner.outcomes.push_back(successfulProbe(
        "Warning: <candidate>/qml/AlertsView.qml:2:1: Unqualified access [unqualified]\n"));
    runner.outcomes.push_back(successfulProbe("Lint passed with no warnings.\n"));

    const auto verification = ManifestProposalSetVerifier::verify(
        request, fixture.service, runner, {});
    require(verification.success(), "Legacy verification call itself failed");
    require(verification.value().checks.front().status
            == ExecutionVerificationStatus::Failed,
        "A pre-policy frozen requirement must preserve historical strict warning semantics");
    require(std::none_of(verification.value().diagnostics.begin(),
                verification.value().diagnostics.end(),
                [](const ManifestVerificationDiagnostic& diagnostic) {
                    return diagnostic.code == "verification.qml_warning_advisory";
                }),
        "A legacy strict requirement must never emit a current-policy advisory diagnostic");
}

void testProjectWideQmlLintPassesOnDebtPlusAdvisoryWithTwoDisjointDiagnostics()
{
    Fixture fixture;
    fixture.add("p1", {
        change("a", FileChangeOperation::Create, "qml/CMakeLists.txt", "# cmake"),
        change("b", FileChangeOperation::Create, "qml/AlertsView.qml", "Item {}")});
    const auto candidateOverlay = ManifestProposalOverlay::build(
        overlayRequest(fixture, {"p1"}), fixture.service);
    require(candidateOverlay.success(), "Fixture candidate overlay failed to build");
    const auto baselineOverlay = buildBaselineOverlay(fixture);

    ExecutionManifestTask owner;
    owner.id = "component";
    owner.objective = "Owns AlertsView.qml";
    owner.role = "component-author";
    owner.requestedCapability = WorkspaceCapability::Propose;
    owner.ownedPaths = {"qml/AlertsView.qml"};
    ManifestProposalSetVerifierRequest request {
        candidateOverlay.value(), {integrationTaskWithoutQmlScope(), owner}, &baselineOverlay};
    FakeCommandRunner runner;
    // Candidate: unchanged debt on AppShell.qml plus a brand-new Advisory
    // finding on the newly-owned AlertsView.qml.
    runner.outcomes.push_back(successfulProbe(
        "Warning: <candidate>/qml/AppShell.qml:1:1: Unqualified access [unqualified]\n"
        "Warning: <candidate>/qml/AlertsView.qml:2:1: Unqualified access [unqualified]\n"));
    runner.outcomes.push_back(successfulProbe(
        "Warning: <candidate>/qml/AppShell.qml:1:1: Unqualified access [unqualified]\n"));

    const auto verification = ManifestProposalSetVerifier::verify(request, fixture.service, runner, {});
    require(verification.success(), "Verification call itself failed");
    const auto& check = verification.value().checks.front();
    require(check.status == ExecutionVerificationStatus::Passed,
        "Unchanged debt plus a new Advisory finding must both be non-blocking together");
    bool sawDebt = false;
    bool sawAdvisory = false;
    for (const auto& diagnostic : check.diagnostics) {
        if (diagnostic.code == "verification.qml_warning_baseline_debt") {
            sawDebt = true;
            require(diagnostic.structuredFindings.size() == 1
                    && diagnostic.structuredFindings.front().fileKey.find("AppShell.qml")
                        != std::string::npos,
                "The debt diagnostic must carry only the unchanged AppShell.qml finding");
        }
        if (diagnostic.code == "verification.qml_warning_advisory") {
            sawAdvisory = true;
            require(diagnostic.structuredFindings.size() == 1
                    && diagnostic.structuredFindings.front().fileKey.find("AlertsView.qml")
                        != std::string::npos,
                "The advisory diagnostic must carry only the new AlertsView.qml finding");
        }
    }
    require(sawDebt && sawAdvisory,
        "Debt and Advisory must be reported as two separate, non-overlapping diagnostics");
}

void testProjectWideQmlLintFailsOnAdvisoryPlusHardMixed()
{
    Fixture fixture;
    fixture.add("p1", {
        change("a", FileChangeOperation::Create, "qml/CMakeLists.txt", "# cmake"),
        change("b", FileChangeOperation::Create, "qml/AlertsView.qml", "Item {}"),
        change("c", FileChangeOperation::Create, "qml/MachinesView.qml", "Item {}")});
    const auto candidateOverlay = ManifestProposalOverlay::build(
        overlayRequest(fixture, {"p1"}), fixture.service);
    require(candidateOverlay.success(), "Fixture candidate overlay failed to build");
    const auto baselineOverlay = buildBaselineOverlay(fixture);

    ExecutionManifestTask alertsOwner;
    alertsOwner.id = "alerts";
    alertsOwner.objective = "Owns AlertsView.qml";
    alertsOwner.role = "component-author";
    alertsOwner.requestedCapability = WorkspaceCapability::Propose;
    alertsOwner.ownedPaths = {"qml/AlertsView.qml"};
    ExecutionManifestTask machinesOwner;
    machinesOwner.id = "machines";
    machinesOwner.objective = "Owns MachinesView.qml";
    machinesOwner.role = "component-author";
    machinesOwner.requestedCapability = WorkspaceCapability::Propose;
    machinesOwner.ownedPaths = {"qml/MachinesView.qml"};
    ManifestProposalSetVerifierRequest request {candidateOverlay.value(),
        {integrationTaskWithoutQmlScope(), alertsOwner, machinesOwner}, &baselineOverlay};
    FakeCommandRunner runner;
    runner.outcomes.push_back(successfulProbe(
        "Warning: <candidate>/qml/AlertsView.qml:2:1: Unqualified access [unqualified]\n"
        "Warning: <candidate>/qml/MachinesView.qml:4:1: Failed to import module [import]\n"));
    runner.outcomes.push_back(successfulProbe("Lint passed with no warnings.\n"));

    const auto verification = ManifestProposalSetVerifier::verify(request, fixture.service, runner, {});
    require(verification.success(), "Verification call itself failed");
    const auto& check = verification.value().checks.front();
    require(check.status == ExecutionVerificationStatus::Failed,
        "A new Hard finding must keep the whole check Failed even alongside an Advisory one");
    require(!requiredAcceptancePassed(verification.value()),
        "A mixed advisory+hard check must never flow through as authoritative Passed");
    bool sawAdvisory = false;
    bool sawHard = false;
    for (const auto& diagnostic : check.diagnostics) {
        if (diagnostic.code == "verification.qml_warning_advisory") {
            sawAdvisory = true;
            require(diagnostic.structuredFindings.size() == 1
                    && diagnostic.structuredFindings.front().fileKey.find("AlertsView.qml")
                        != std::string::npos,
                "The advisory diagnostic must carry only the Advisory finding");
        }
        if (diagnostic.code == "verification.qml_warning") {
            sawHard = true;
            require(diagnostic.structuredFindings.size() == 1
                    && diagnostic.structuredFindings.front().fileKey.find("MachinesView.qml")
                        != std::string::npos,
                "The blocking diagnostic must carry only the Hard finding");
        }
    }
    require(sawAdvisory && sawHard,
        "Both severities must be reported separately, with no finding duplicated between buckets");
}

void testProjectWideQmlLintFailsOnAdvisoryPlusOutsideManifest()
{
    Fixture fixture;
    fixture.add("p1", {
        change("a", FileChangeOperation::Create, "qml/CMakeLists.txt", "# cmake"),
        change("b", FileChangeOperation::Create, "qml/AlertsView.qml", "Item {}")});
    const auto candidateOverlay = ManifestProposalOverlay::build(
        overlayRequest(fixture, {"p1"}), fixture.service);
    require(candidateOverlay.success(), "Fixture candidate overlay failed to build");
    const auto baselineOverlay = buildBaselineOverlay(fixture);

    ExecutionManifestTask owner;
    owner.id = "alerts";
    owner.objective = "Owns AlertsView.qml";
    owner.role = "component-author";
    owner.requestedCapability = WorkspaceCapability::Propose;
    owner.ownedPaths = {"qml/AlertsView.qml"};
    ManifestProposalSetVerifierRequest request {
        candidateOverlay.value(), {integrationTaskWithoutQmlScope(), owner}, &baselineOverlay};
    FakeCommandRunner runner;
    // The Advisory-tagged finding on the unowned qml/Unowned.qml must stay
    // outside-Manifest/blocking - an Advisory tag never substitutes for real
    // Manifest ownership.
    runner.outcomes.push_back(successfulProbe(
        "Warning: <candidate>/qml/AlertsView.qml:2:1: Unqualified access [unqualified]\n"
        "Warning: <candidate>/qml/Unowned.qml:3:1: Unqualified access [unqualified]\n"));
    runner.outcomes.push_back(successfulProbe("Lint passed with no warnings.\n"));

    const auto verification = ManifestProposalSetVerifier::verify(request, fixture.service, runner, {});
    require(verification.success(), "Verification call itself failed");
    const auto& check = verification.value().checks.front();
    require(check.status == ExecutionVerificationStatus::Failed,
        "An outside-Manifest finding must keep the check Failed regardless of any Advisory finding");
    bool sawAdvisory = false;
    bool sawOutside = false;
    for (const auto& diagnostic : check.diagnostics) {
        if (diagnostic.code == "verification.qml_warning_advisory") sawAdvisory = true;
        if (diagnostic.code == "verification.qml_warning_outside_manifest") {
            sawOutside = true;
            require(diagnostic.structuredFindings.size() == 1
                    && diagnostic.structuredFindings.front().fileKey.find("Unowned.qml")
                        != std::string::npos,
                "The outside-Manifest diagnostic must name its exact unowned path");
        }
    }
    require(sawAdvisory && sawOutside,
        "The in-scope Advisory finding and the outside-Manifest finding must both be reported");
}

void testScopedQmlLintAlsoPassesOnUnchangedBaselineDebt()
{
    Fixture fixture;
    fixture.add("p1", {change("a", FileChangeOperation::Create,
        "qml/AppSidebar.qml", "Item {}")});
    const auto candidateOverlay = ManifestProposalOverlay::build(
        overlayRequest(fixture, {"p1"}), fixture.service);
    require(candidateOverlay.success(), "Fixture candidate overlay failed to build");
    const auto baselineOverlay = buildBaselineOverlay(fixture);

    ExecutionAcceptanceRequirement lint;
    lint.id = "lint";
    lint.description = "Scoped QML lint";
    lint.verificationKind = ExecutionVerificationKind::QmlLint;
    lint.verificationArguments = {{"buildDirectory", "build"}, {"target", "all_qmllint"}};
    auto task = taskWithRequirement(lint);
    task.ownedPaths = {"qml/AppSidebar.qml"};
    ManifestProposalSetVerifierRequest request {
        candidateOverlay.value(), {task}, &baselineOverlay};
    FakeCommandRunner runner;
    const auto warning = successfulProbe(
        "Warning: <candidate>/qml/AppSidebar.qml:7:2: Unqualified access [unqualified]\n");
    runner.outcomes = {warning, warning};

    const auto verification = ManifestProposalSetVerifier::verify(
        request, fixture.service, runner, {});
    require(verification.success()
            && verification.value().checks.front().status
                == ExecutionVerificationStatus::Passed,
        "A scoped task must use the same baseline-delta policy as an integration task");
}

void testBaselineLintRunsOnlyAfterItsOwnRequiredConfigureAndBuild()
{
    Fixture fixture;
    fixture.add("p1", {change("a", FileChangeOperation::Create,
        "qml/AppSidebar.qml", "Item {}")});
    const auto candidateOverlay = ManifestProposalOverlay::build(
        overlayRequest(fixture, {"p1"}), fixture.service);
    require(candidateOverlay.success(), "Fixture candidate overlay failed to build");
    const auto baselineOverlay = buildBaselineOverlay(fixture);

    ExecutionAcceptanceRequirement build;
    build.id = "build";
    build.description = "Configure and build";
    build.verificationKind = ExecutionVerificationKind::CMakeBuild;
    build.verificationArguments = {{"preset", "local"}, {"buildPreset", "local"},
        {"buildDirectory", "build"}};
    ExecutionAcceptanceRequirement lint;
    lint.id = "lint";
    lint.description = "QML lint";
    lint.verificationKind = ExecutionVerificationKind::QmlLint;
    lint.verificationArguments = {{"buildDirectory", "build"}, {"target", "all_qmllint"}};
    auto task = taskWithRequirement(build);
    task.acceptanceContract->requirements.push_back(lint);
    task.ownedPaths = {"qml/AppSidebar.qml"};

    ManifestProposalSetVerifierRequest request {
        candidateOverlay.value(), {task}, &baselineOverlay};
    FakeCommandRunner runner;
    const auto clean = successfulProbe("ok\n");
    const auto warning = successfulProbe(
        "Warning: <candidate>/qml/AppSidebar.qml:7:2: Unqualified access [unqualified]\n");
    runner.outcomes = {clean, clean, warning, clean, clean, warning};

    const auto verification = ManifestProposalSetVerifier::verify(
        request, fixture.service, runner, {});
    require(verification.success() && runner.requests.size() == 6,
        "Candidate plus baseline must each execute configure, build, then lint");
    require(runner.requests[0].workingDirectory == candidateOverlay.value().root
            && runner.requests[1].workingDirectory == candidateOverlay.value().root
            && runner.requests[2].workingDirectory == candidateOverlay.value().root
            && runner.requests[3].workingDirectory == baselineOverlay.root
            && runner.requests[4].workingDirectory == baselineOverlay.root
            && runner.requests[5].workingDirectory == baselineOverlay.root,
        "Baseline lint must never reuse the candidate build tree or run before its own prerequisite");
    require(verification.value().checks.back().status == ExecutionVerificationStatus::Passed,
        "Identical warning evidence must pass once both roots were independently prepared");
}

void testVerifierRejectsMismatchedBaselineIdentityBeforeCommands()
{
    Fixture fixture;
    fixture.add("p1", {change("a", FileChangeOperation::Create,
        "qml/CMakeLists.txt", "# cmake")});
    const auto candidateOverlay = ManifestProposalOverlay::build(
        overlayRequest(fixture, {"p1"}), fixture.service);
    require(candidateOverlay.success(), "Fixture candidate overlay failed to build");
    auto baselineOverlay = buildBaselineOverlay(fixture);
    baselineOverlay.workspaceIdentity = "other-project";
    ManifestProposalSetVerifierRequest request {
        candidateOverlay.value(), {integrationTaskWithoutQmlScope()}, &baselineOverlay};
    FakeCommandRunner runner;
    const auto verification = ManifestProposalSetVerifier::verify(
        request, fixture.service, runner, {});
    require(!verification.success()
            && verification.error()->code == "verification.baseline_identity_mismatch"
            && runner.calls == 0,
        "A foreign baseline identity must fail before any verification command runs");
}

void testProjectWideQmlLintUnchangedBehaviorWithoutBaselineOverlay()
{
    // Backward-compatibility guard: every existing caller that does not
    // populate ManifestProposalSetVerifierRequest::baselineOverlay must see
    // byte-for-byte the same historical fail-closed behavior as before this
    // hardening - the exact scenario the whole feature is meant to fix stays
    // unfixed for a caller that never opts in.
    Fixture fixture;
    fixture.add("p1", {change("a", FileChangeOperation::Create, "qml/CMakeLists.txt", "# cmake")});
    const auto candidateOverlay = ManifestProposalOverlay::build(
        overlayRequest(fixture, {"p1"}), fixture.service);
    require(candidateOverlay.success(), "Fixture candidate overlay failed to build");

    ManifestProposalSetVerifierRequest request {
        candidateOverlay.value(), {integrationTaskWithoutQmlScope()}};
    require(request.baselineOverlay == nullptr, "This request must not opt into baseline awareness");
    FakeCommandRunner runner;
    runner.outcomes.push_back(successfulProbe(
        "Warning: <candidate>/qml/AppShell.qml:1:1: Unqualified access [unqualified]\n"));

    const auto verification = ManifestProposalSetVerifier::verify(request, fixture.service, runner, {});
    require(verification.success(), "Verification call itself failed");
    require(verification.value().checks.front().status == ExecutionVerificationStatus::Failed,
        "Without an opted-in baseline overlay, the historical fail-closed rule must be unchanged");
    require(runner.calls == 1,
        "Without a baseline overlay, only the single candidate lint command may run");
}

// QML-lint advisory-severity policy (2026-09-28): verification.qml_warning_advisory
// is a plain string diagnostic code on the existing generic
// ManifestVerificationDiagnostic/Finding shape - no schema migration, no
// second parallel model. Round-trips through the exact same JSON codec every
// other diagnostic code already uses.
void testVerificationQmlWarningAdvisoryDiagnosticRoundTripsThroughJson()
{
    ManifestProposalSetVerification verification;
    verification.workspaceIdentity = "workspace-1";
    verification.baselineFingerprint = "baseline-1";
    verification.overlayFingerprint = "overlay-1";
    verification.proposalSafetyStatus = ExecutionVerificationStatus::Passed;
    verification.buildLintTestStatus = ExecutionVerificationStatus::Passed;
    verification.acceptanceStatus = ExecutionVerificationStatus::Passed;
    ManifestVerificationCheckResult check;
    check.profileId = "lint";
    check.kind = ExecutionVerificationKind::QmlLint;
    check.status = ExecutionVerificationStatus::Passed;
    check.commandIdentity = "cmake.qml_lint_target";
    check.boundedOutputHash = std::string(64, 'a');
    ManifestVerificationDiagnostic advisory;
    advisory.code = "verification.qml_warning_advisory";
    advisory.message = "QML lint emitted one or more advisory-severity warnings that do not "
        "block this check";
    advisory.requirementId = "lint";
    advisory.structuredFindings = {
        qmlFinding("qml/AlertsView.qml", 2, 1, "unqualified", "Unqualified access")};
    check.diagnostics = {advisory};
    verification.checks = {check};
    verification.diagnostics = {advisory};
    verification.criteria = {{"lint", true, ExecutionVerificationStatus::Passed, {}}};

    const auto json = renderManifestProposalSetVerificationJson(verification);
    const auto parsed = parseManifestProposalSetVerificationJson(json);
    require(parsed.success(), "A verification carrying verification.qml_warning_advisory must "
        "parse back without error");
    require(parsed.value().diagnostics.size() == 1
            && parsed.value().diagnostics.front().code == "verification.qml_warning_advisory"
            && parsed.value().diagnostics.front().structuredFindings.size() == 1
            && parsed.value().diagnostics.front().structuredFindings.front().tag == "unqualified"
            && parsed.value().diagnostics.front().structuredFindings.front().fileKey
                == "<candidate>/qml/AlertsView.qml",
        "The advisory diagnostic and its structured finding must survive the JSON round trip "
        "byte-for-byte");
    require(usesBaselineAwareQmlLintPolicy(parsed.value()),
        "verification.qml_warning_advisory must be recognized as a current-policy marker so a "
        "later continuation/repair never misreconstructs this row as pre-baseline-aware legacy");
    require(requiredAcceptancePassed(parsed.value()),
        "An advisory-only round-tripped verification must still read as authoritative Passed");
}

// QML-lint advisory-severity policy (2026-09-28): the new diagnostic code
// must be ignorable-non-repair, exactly like baseline_debt/outside_task_scope
// - it never independently blocks continuation/manual-handoff eligibility
// for a genuinely Failed mixed (Hard+Advisory) result, and a worker has
// nothing of its own to fix for an Advisory-only entry.
void testIsIgnorableNonRepairDiagnosticCodeRecognizesQmlWarningAdvisory()
{
    require(isIgnorableNonRepairDiagnosticCode("verification.qml_warning_advisory"),
        "verification.qml_warning_advisory must be an ignorable-non-repair diagnostic code");
    require(!isContentRepairableDiagnosticCode("verification.qml_warning_advisory"),
        "verification.qml_warning_advisory must not also be listed as content-repairable - it "
        "has no content of its own for a worker to fix");
}

} // namespace

int main()
{
    try {
        testOverlayMaterializesAllOperationsAndPreservesWorkspace();
        testOverlayExcludesUnselectedSiblingAndFailsMissingContent();
        testOverlayCapsAndCleanupContainmentFailClosed();
        testOverlayUsesPolicyVisibleSnapshotAndRejectsSourceDrift();
        testCombinedConflictAndWarningBearingQmlLintFail();
        testCorrectedFixturePassesAndMissingToolIsNotVerifiable();
        testFailedFreshConfigureSkipsLintAndSanitizesDiagnostic();
        testInvalidProfileArgumentsAreNotVerifiableNotHardFailure();
        testVerificationPreflightRejectsMissingAndUnknownPresets();
        testVerificationPreflightRequiresCleanConfigureForQmlLint();
        testVerificationPreflightAcceptsAvailableProfiles();
        testSystemCommandRunnerDrainsShortProcessOutput();
        testConcurrentOverlayReconstructionStress1000();
        testDefaultOverlayRootIsSignificantlyShorterThanOld();
        testCandidateDirectoriesAreUnique();
        testScratchRootDisjointFromWorkspaceIsEnforced();
        testCallerProvidedScratchRootIsHonored();
        testReparsePointScratchRootIsRejected();
        testPathBudgetOverflowFailsClosedBeforeAnyCommand();
        testRealisticQtBuildDirectoryShapeTripsPathBudget();
        testCMakeObjectPathMaxMarkerStopsBeforeBuild();
        testNoisyMsvcOutputSurfacesFatalErrorNotCommandLine();
        testQmlWarningExcerptContainsTypeNameDiagnostic();
        testBoundedExcerptCoversMultipleFiles();
        testStructuredFindingsCoverEveryAffectedFileWithCorrectLocation();
        testStructuredFindingsSkipFilelessContinuationLine();
        testStructuredFindingsStripInfoSeverityPrefix();
        testStructuredFindingsRecoverDiagnosticAfterConcatenatedQmlLintHint();
        testStructuredFindingsEmptyWhenLineHasNoLocationShape();
        testBoundedExcerptGuaranteesEveryFileWhenMiddleClusterWouldBeDropped();
        testBoundedExcerptRepresentsEarlyAndLateLinesWithTruncationMarker();
        testBoundedExcerptDeduplicatesRepeatedLines();
        testBoundedExcerptStillFiltersAbsolutePathsAndSecrets();
        testBoundedExcerptPrioritizesFatalErrorOverMultipleNoisyCommandLines();
        testBoundedExcerptCombinesFatalAndQmlDiagnosticTiers();
        testBoundedExcerptNeverExceedsByteCapEvenWithMarker();
        testQmlLintCommandRunsAgainstCandidateOverlayNotRealWorkspace();
        testCandidateModuleImportWarningStillFailsVerification();
        testQmlWarningsOutsideFrozenTaskWriteScopeDoNotBlockTaskAcceptance();
        testQmlWarningInFrozenTaskWriteScopeStillFails();
        testQmlWarningsOutsideFrozenDependencyOutputScopeDoNotBlockIntegrationTask();
        testQmlWarningInFrozenDependencyOutputScopeStillFailsIntegrationTask();
        testBaselineOnlyQmlReadDoesNotCreateVerificationScope();
        testUnresolvedQmlReadDoesNotCreateVerificationScope();
        testUnattributedQmlWarningStillFailsClosed();
        testClassifyQmlLintFindingSeverityRecognizesUnqualifiedAndUnusedImportsAsAdvisory();
        testClassifyQmlLintFindingMakesLoadBreakingMessagesHardUnderAnyTag();
        testFingerprintQmlLintFindingIgnoresLineAndColumn();
        testFingerprintQmlLintFindingCanonicalizesFileKeyAcrossCandidateWrapperCaseAndSeparators();
        testComputeQmlLintDiagnosticDeltaClassifiesEliminatedNewAndRetainedAcrossLineMovement();
        testComputeQmlLintDiagnosticDeltaUsesMultisetSemanticsForDuplicateFingerprints();
        testClassifyManifestContentOutcomeRecognizesBoundedAdvisoryLintFailureAsNeedsManualAction();
        testClassifyManifestContentOutcomeTreatsMissingSignalHandlerAsHardNotManualAction();
        testClassifyManifestContentOutcomeFailsClosedWhenProposalSafetyFailed();
        testClassifyManifestContentOutcomeFailsClosedWhenANonLintCheckFailed();
        testClassifyManifestContentOutcomeFailsClosedOnAnyHardLintFinding();
        testClassifyManifestContentOutcomeIgnoresHardBaselineDebtBesideRepairableWarning();
        testPrimaryFailureDiagnosticSkipsNonBlockingDebtAndAdvisory();
        testRenderQmlLintFindingLabelUsesDisplayPathAndNeverLeaksAnAbsolutePath();
        testDescribeDiagnosticFindingsIsBoundedAndSkipsNonBlockingAndRepeatedMessages();
        testRenderVerificationFailureReasonAppendsTheBlockingFindings();
        testRenderVerificationDiagnosticLinesNamesBlockingFindingsAndOnlyCountsNonBlocking();
        testClassifyManifestContentOutcomeFailsClosedWhenFailedLintHasNoStructuredEvidence();
        testClassifyManifestContentOutcomeFailsClosedOnNotVerifiableQmlLintEvenWithAdvisoryFindings();
        testClassifyManifestContentOutcomeIsUndifferentiatedWhenNothingFailed();
        testResolveQmlLintCriterionDeltaPassesCleanBaselineAndCandidate();
        testResolveQmlLintCriterionDeltaTreatsRetainedFindingAsPreExistingDebt();
        testResolveQmlLintCriterionDeltaOmitsResolvedBaselineDebtFromEntries();
        testResolveQmlLintCriterionDeltaFailsOnNewInScopeFinding();
        testResolveQmlLintCriterionDeltaFailsOnNewOutsideManifestFinding();
        testResolveQmlLintCriterionDeltaRelocatedFindingStaysDebt();
        testResolveQmlLintCriterionDeltaExtraDuplicateInstanceIsNewFinding();
        testResolveQmlLintCriterionDeltaUnmappablePathIsUnattributable();
        testResolveQmlLintCriterionDeltaFailsClosedWhenBaselineEvidenceUnavailable();
        testResolveQmlLintCriterionDeltaFailsClosedOnRetainedUnsafePath();
        testResolveQmlLintCriterionDeltaPassesOnNewInScopeAdvisoryFinding();
        testResolveQmlLintCriterionDeltaRecognizesUnusedImportsAsAdvisoryInScope();
        testResolveQmlLintCriterionDeltaStillFailsOnNewOutsideManifestAdvisoryFinding();
        testResolveQmlLintCriterionDeltaStillFailsOnUnattributableAdvisoryFinding();
        testResolveQmlLintCriterionDeltaStillFailsWhenBaselineUnavailableForAdvisoryFinding();
        testResolveQmlLintCriterionDeltaMixedAdvisoryAndHardInScopeFindingsFails();
        testResolveQmlLintCriterionDeltaDuplicateAdvisoryInstancesAllCountedAsNew();
        testResolveQmlLintCriterionDeltaFailsOnNewMissingSignalHandlerUnderAdvisoryTag();
        testProjectWideQmlLintPassesWhenBaselineAndCandidateBothClean();
        testProjectWideQmlLintPassesOnUnchangedBaselineDebt();
        testProjectWideQmlLintComparesCompleteSetsNotTruncatedSamples();
        testPrecomputedBaselineVerificationCarriesCompleteQmlFindings();
        testQmlLintFailsClosedWhenFindingsExceedComparableBound();
        testProjectWideQmlLintPassesWhenBaselineWarningResolved();
        testProjectWideQmlLintFailsOnNewInScopeWarning();
        testProjectWideQmlLintFailsOnNewOutsideManifestWarning();
        testProjectWideQmlLintMixedDebtAndNewWarningFailsOnlyForNew();
        testProjectWideQmlLintFailsClosedWhenBaselineProbeToolUnavailable();
        testProjectWideQmlLintPassesOnAdvisoryOnlyNewWarning();
        testProjectWideQmlLintFailsOnNewMissingSignalHandlerDespiteAdvisoryTag();
        testLegacyQmlLintRequirementWithoutFrozenPolicyKeepsAdvisoryTagBlocking();
        testProjectWideQmlLintPassesOnDebtPlusAdvisoryWithTwoDisjointDiagnostics();
        testProjectWideQmlLintFailsOnAdvisoryPlusHardMixed();
        testProjectWideQmlLintFailsOnAdvisoryPlusOutsideManifest();
        testScopedQmlLintAlsoPassesOnUnchangedBaselineDebt();
        testBaselineLintRunsOnlyAfterItsOwnRequiredConfigureAndBuild();
        testVerifierRejectsMismatchedBaselineIdentityBeforeCommands();
        testProjectWideQmlLintUnchangedBehaviorWithoutBaselineOverlay();
        testVerificationQmlWarningAdvisoryDiagnosticRoundTripsThroughJson();
        testIsIgnorableNonRepairDiagnosticCodeRecognizesQmlWarningAdvisory();
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "Manifest proposal verification tests failed: "
                  << exception.what() << '\n';
        return 1;
    }
}
