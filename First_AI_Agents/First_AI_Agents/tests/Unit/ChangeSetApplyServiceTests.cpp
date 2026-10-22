// Stage 8 tests for ChangeSetApplyService: hash-verified, rollback-safe
// workspace apply pipeline.  Uses real FileChangeProposalStore + temp
// directories so file I/O paths are fully exercised without mocking.
#include "ai/application/ChangeProposalService.hpp"
#include "ai/application/ChangeSetApplyService.hpp"
#include "ai/application/ArtifactService.hpp"
#include "ai/application/FilesystemPathUtils.hpp"
#include "ai/application/IChangeApprovalRepository.hpp"
#include "ai/application/IChangeProposalRepository.hpp"
#include "ai/application/IWorkflowRunner.hpp"
#include "ai/application/InMemoryRepositories.hpp"
#include "ai/application/Sha256.hpp"
#include "ai/application/WorkspaceBaselineProvenance.hpp"
#include "ai/infrastructure/FileChangeProposalStore.hpp"
#include "RecordingOutcomeBriefTrigger.hpp"

#include <algorithm>
#include <cassert>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

namespace fs = std::filesystem;
namespace common = ai::common;
using namespace ai::application;
using namespace ai::infrastructure;

void req(bool cond, const char* msg)
{
    if (!cond) throw std::runtime_error(msg);
}

struct TempDir {
    fs::path path;
    TempDir()
    {
        path = fs::temp_directory_path()
            / ("ai-orch-apply-test-"
                + std::to_string(
                    std::chrono::steady_clock::now().time_since_epoch().count()));
        fs::create_directories(path);
    }
    ~TempDir()
    {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

void writeFile(const fs::path& p, const std::string& content)
{
    fs::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f.write(content.data(), static_cast<std::streamsize>(content.size()));
}

std::string readFile(const fs::path& p)
{
    std::ifstream f(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f), {});
}

// fs::exists()/fs::equivalent() can't distinguish "Main.qml" from "main.qml"
// on a case-insensitive filesystem - this scans the real directory entries
// to find the on-disk casing actually used for a case-insensitive match of
// expectedName, so a case-only rename can be verified to have really taken
// effect rather than silently no-oped.
std::string actualFilenameCasing(const fs::path& dir, const std::string& expectedName)
{
    std::string lowerExpected = expectedName;
    std::transform(lowerExpected.begin(), lowerExpected.end(), lowerExpected.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    for (const auto& entry : fs::directory_iterator(dir)) {
        std::string name = entry.path().filename().string();
        std::string lowerName = name;
        std::transform(lowerName.begin(), lowerName.end(), lowerName.begin(),
            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (lowerName == lowerExpected) return name;
    }
    return "";
}

bool hasLeftoverRenameTempFile(const fs::path& dir)
{
    for (const auto& entry : fs::directory_iterator(dir)) {
        if (entry.path().filename().string().rfind(".rename-temp-", 0) == 0) return true;
    }
    return false;
}

class FakeChangeProposalRepository final : public IChangeProposalRepository {
public:
    common::Result<void> insertChangeSet(const ProposedChangeSet& cs) override
    {
        std::lock_guard lock(mu_);
        if (byId_.count(cs.changeSetId))
            return common::Result<void>::failure({"proposal.duplicate_id", "Duplicate"});
        byId_[cs.changeSetId] = cs;
        changeLists_[cs.changeSetId] = cs.changes;
        findingLists_[cs.changeSetId] = cs.validationFindings;
        byId_[cs.changeSetId].changes.clear();
        byId_[cs.changeSetId].validationFindings.clear();
        return common::Result<void>::ok();
    }
    common::Result<ProposedChangeSet> updateChangeSetState(
        const ChangeSetId& id, ChangeSetState newState) override
    {
        std::lock_guard lock(mu_);
        const auto it = byId_.find(id);
        if (it == byId_.end())
            return common::Result<ProposedChangeSet>::failure(
                {"proposal.not_found", "Not found"});
        it->second.state = newState;
        return common::Result<ProposedChangeSet>::ok(it->second);
    }
    common::Result<void> markMissingEvidence(
        const ChangeSetId& id, std::string code, std::string message) override
    {
        std::lock_guard lock(mu_);
        const auto it = byId_.find(id);
        if (it == byId_.end())
            return common::Result<void>::failure({"proposal.not_found", "Not found"});
        it->second.missingEvidenceCode = std::move(code);
        it->second.missingEvidenceMessage = std::move(message);
        return common::Result<void>::ok();
    }
    common::Result<std::optional<ProposedChangeSet>> findChangeSet(
        const ChangeSetId& id) const override
    {
        std::lock_guard lock(mu_);
        const auto it = byId_.find(id);
        if (it == byId_.end())
            return common::Result<std::optional<ProposedChangeSet>>::ok(std::nullopt);
        return common::Result<std::optional<ProposedChangeSet>>::ok(it->second);
    }
    common::Result<std::vector<ProposedChangeSet>> listForRun(
        const RunId& runId) const override
    {
        std::lock_guard lock(mu_);
        std::vector<ProposedChangeSet> r;
        for (const auto& [id, cs] : byId_) if (cs.runId == runId) r.push_back(cs);
        return common::Result<std::vector<ProposedChangeSet>>::ok(std::move(r));
    }
    common::Result<std::vector<ProposedChangeSet>> listForProject(
        const ProjectId&) const override
    {
        return common::Result<std::vector<ProposedChangeSet>>::ok({});
    }
    common::Result<std::vector<ProposedFileChange>> listChanges(
        const ChangeSetId& id) const override
    {
        std::lock_guard lock(mu_);
        const auto it = changeLists_.find(id);
        if (it == changeLists_.end())
            return common::Result<std::vector<ProposedFileChange>>::ok({});
        return common::Result<std::vector<ProposedFileChange>>::ok(it->second);
    }
    common::Result<std::vector<ValidationFinding>> listValidationFindings(
        const ChangeSetId& id) const override
    {
        std::lock_guard lock(mu_);
        const auto it = findingLists_.find(id);
        if (it == findingLists_.end())
            return common::Result<std::vector<ValidationFinding>>::ok({});
        return common::Result<std::vector<ValidationFinding>>::ok(it->second);
    }
    common::Result<std::unordered_map<ChangeSetId, std::vector<ProposedFileChange>>>
    listChangesForSets(const std::vector<ChangeSetId>& ids) const override
    {
        std::lock_guard lock(mu_);
        std::unordered_map<ChangeSetId, std::vector<ProposedFileChange>> result;
        for (const auto& id : ids) {
            const auto it = changeLists_.find(id);
            if (it != changeLists_.end()) result[id] = it->second;
        }
        return common::Result<std::unordered_map<ChangeSetId, std::vector<ProposedFileChange>>>::ok(
            std::move(result));
    }
    common::Result<std::unordered_map<ChangeSetId, std::vector<ValidationFinding>>>
    listValidationFindingsForSets(const std::vector<ChangeSetId>& ids) const override
    {
        std::lock_guard lock(mu_);
        std::unordered_map<ChangeSetId, std::vector<ValidationFinding>> result;
        for (const auto& id : ids) {
            const auto it = findingLists_.find(id);
            if (it != findingLists_.end()) result[id] = it->second;
        }
        return common::Result<std::unordered_map<ChangeSetId, std::vector<ValidationFinding>>>::ok(
            std::move(result));
    }

    // Test helper: directly insert a change with given state, bypassing validation.
    void insertRaw(ProposedChangeSet cs, std::vector<ProposedFileChange> changes)
    {
        std::lock_guard lock(mu_);
        const auto id = cs.changeSetId;
        changeLists_[id] = std::move(changes);
        cs.changes.clear();
        byId_[id] = std::move(cs);
    }

private:
    mutable std::mutex mu_;
    std::map<ChangeSetId, ProposedChangeSet> byId_;
    std::map<ChangeSetId, std::vector<ProposedFileChange>> changeLists_;
    std::map<ChangeSetId, std::vector<ValidationFinding>> findingLists_;
};

class FakeChangeApprovalRepository final : public IChangeApprovalRepository {
public:
    common::Result<void> recordApproval(const ApprovalRecord& record) override
    {
        std::lock_guard lock(mu_);
        if (byId_.count(record.changeSetId))
            return common::Result<void>::failure({"proposal.duplicate_id", "Duplicate"});
        byId_[record.changeSetId] = record;
        return common::Result<void>::ok();
    }
    common::Result<std::optional<ApprovalRecord>> findApproval(
        const ChangeSetId& changeSetId) const override
    {
        std::lock_guard lock(mu_);
        const auto it = byId_.find(changeSetId);
        if (it == byId_.end())
            return common::Result<std::optional<ApprovalRecord>>::ok(std::nullopt);
        return common::Result<std::optional<ApprovalRecord>>::ok(it->second);
    }
    common::Result<void> recordApplyOutcome(
        const ChangeSetId& changeSetId, const std::string& result,
        const std::optional<std::string>& errorCode,
        const std::vector<ApplyHashEntry>& hashLog,
        const std::optional<std::string>& postApplyWorkspaceFingerprint) override
    {
        std::lock_guard lock(mu_);
        const auto it = byId_.find(changeSetId);
        if (it == byId_.end())
            return common::Result<void>::failure({"proposal.not_found", "No approval record"});
        it->second.appliedAt = std::chrono::system_clock::now();
        it->second.applyResult = result;
        it->second.applyErrorCode = errorCode;
        it->second.applyHashLog = hashLog;
        it->second.postApplyWorkspaceFingerprint = postApplyWorkspaceFingerprint;
        return common::Result<void>::ok();
    }

    // Test helper: force a mismatched hash to simulate a corrupted/stale
    // approval record without going through the (deliberately immutable)
    // normal insert path.
    void overwriteSnapshotHashForTest(const ChangeSetId& changeSetId, std::string hash)
    {
        std::lock_guard lock(mu_);
        byId_.at(changeSetId).snapshotHash = std::move(hash);
    }

private:
    mutable std::mutex mu_;
    std::map<ChangeSetId, ApprovalRecord> byId_;
};

// Builds a proposal-submission.v2 JSON for a Create operation. schema/
// agent_role/base_revision are trusted-caller metadata now (see
// TrustedChangeSetMetadata) and were never valid top-level provider fields;
// this contract's exact top-level field set is schemaVersion/summary/changes.
std::string makeCreateProposal(const std::string& relativePath,
                               const std::string& content)
{
    // Escape the content for JSON (basic ASCII, no special chars in tests).
    // Real JSON escaping not needed for these simple ASCII test strings.
    return R"(```json
{
  "schemaVersion": "proposal-submission.v2",
  "summary": "test create",
  "changes": [
    {
      "change_id": "c1",
      "operation": "create",
      "relative_path": ")" + relativePath + R"(",
      "source_path": null,
      "proposed_content": ")" + content + R"("
    }
  ]
}
```)";
}

// No expected_original_hash field: proposal-submission.v2 has none. The
// caller must separately bind req.requireHashEvidence/observedFileHashes
// (see bindHashEvidence below) with currentContent's hash for the resulting
// proposal to reach ReadyForReview - exactly mirroring how
// AgentToolTurnLoopRunner::preflightTrustedHashes populates it in production.
std::string makeModifyProposal(const std::string& relativePath,
                               const std::string&,
                               const std::string& newContent)
{
    return R"(```json
{
  "schemaVersion": "proposal-submission.v2",
  "summary": "test modify",
  "changes": [
    {
      "change_id": "m1",
      "operation": "modify",
      "relative_path": ")" + relativePath + R"(",
      "source_path": null,
      "proposed_content": ")" + newContent + R"("
    }
  ]
}
```)";
}

std::string makeDeleteProposal(const std::string& relativePath,
                               const std::string&)
{
    return R"(```json
{
  "schemaVersion": "proposal-submission.v2",
  "summary": "test delete",
  "changes": [
    {
      "change_id": "d1",
      "operation": "delete",
      "relative_path": ")" + relativePath + R"(",
      "source_path": null,
      "proposed_content": null
    }
  ]
}
```)";
}

std::string makeRenameProposal(const std::string& sourcePath,
                               const std::string& targetPath,
                               const std::string&)
{
    return R"(```json
{
  "schemaVersion": "proposal-submission.v2",
  "summary": "test rename",
  "changes": [
    {
      "change_id": "rn1",
      "operation": "rename",
      "relative_path": ")" + targetPath + R"(",
      "source_path": ")" + sourcePath + R"(",
      "proposed_content": null
    }
  ]
}
```)";
}

// Mirrors ChangeProposalService::bindObservedHashes()'s production
// contract: the Backend's own hash preflight, never the model, is the only
// source of a Modify/Delete/Rename's trusted expectedOriginalHash under
// proposal-submission.v2. subjectPath is relativePath for Modify/Delete, or
// the rename source for Rename.
void bindHashEvidence(
    CreateChangeProposalRequest& req,
    const std::string& subjectPath,
    const std::string& currentContent)
{
    req.requireHashEvidence = true;
    req.observedFileHashes[detail::normalizedPathKey(fs::path(subjectPath))] =
        sha256Hex(currentContent);
}

struct Fixture {
    TempDir projectDir;   // simulated project workspace
    TempDir proposalDir;  // FileChangeProposalStore root
    FakeChangeProposalRepository repo;
    FakeChangeApprovalRepository approvals;
    InMemoryProjectRepository projects;
    InMemoryRunRepository runs;
    InMemoryArtifactRepository artifactRepository;
    InMemoryArtifactStore artifactStore;
    ArtifactService artifactService;
    std::unique_ptr<FileChangeProposalStore> store;

    Fixture() : artifactService(artifactRepository, artifactStore)
    {
        auto storeResult = FileChangeProposalStore::open(proposalDir.path / "proposals");
        assert(storeResult.success());
        store = std::move(storeResult.value());

        Project project;
        project.id = "p1";
        project.name = "Apply fixture";
        project.rootPath = fs::weakly_canonical(projectDir.path);
        project.status = ProjectStatus::Active;
        project.createdAt = std::chrono::system_clock::now();
        project.updatedAt = project.createdAt;
        assert(projects.insert(project).success());

        Run run;
        run.id = "r1";
        run.projectId = project.id;
        run.title = "Apply fixture run";
        run.task = "Exercise safe apply";
        run.workflowId = kExecutionProfileWorkflowId;
        run.workspacePath = project.rootPath;
        run.status = RunStatus::Completed;
        run.createdAt = project.createdAt;
        assert(runs.insertRun(run).success());
    }

    ChangeProposalService makeProposalService()
    {
        return ChangeProposalService(repo, *store);
    }

    ChangeSetApplyService makeApplyService()
    {
        return ChangeSetApplyService(repo, *store, runs, projects, approvals);
    }

    ChangeSetApplyService makeApplyServiceWithArtifacts()
    {
        return ChangeSetApplyService(
            repo, *store, runs, projects, approvals, &artifactService);
    }

    ProjectWritePolicy defaultPolicy()
    {
        ProjectWritePolicy p;
        p.projectRoot = projectDir.path;
        return p;
    }
};

// --- Test cases ---

void testApplyNotFound()
{
    Fixture f;
    auto applyService = f.makeApplyService();

    const auto result = applyService.apply("nonexistent-id");
    req(!result.success(), "should fail for unknown changeSetId");
    req(result.error()->code == "proposal.not_found", "wrong error code");
}

void testApplyNotReady()
{
    Fixture f;
    auto proposalService = f.makeProposalService();

    // A Rejected proposal cannot be applied.
    CreateChangeProposalRequest req_;
    req_.runId      = "r1";
    req_.projectId  = "p1";
    req_.providerId = "prov1";
    req_.modelId    = "m1";
    req_.agentRole  = "test";
    // Use a path that doesn't exist -> validator produces Rejected + hash_mismatch
    req_.rawModelOutput = makeModifyProposal("missing.txt", "x", "y");
    req_.policy         = f.defaultPolicy();

    const auto cs = proposalService.createFromModelOutput(std::move(req_));
    req(cs.success(), "createFromModelOutput should succeed (storing Rejected)");
    req(cs.value().state == ChangeSetState::Rejected, "expected Rejected state");

    auto applyService = f.makeApplyService();
    const auto result = applyService.apply(cs.value().changeSetId);
    req(!result.success(), "apply of Rejected proposal should fail");
    req(result.error()->code == "proposal.not_ready", "wrong error code");
}

void testApplyRejectsMissingOwningRun()
{
    Fixture f;

    // A persisted proposal cannot choose its own workspace. If its owning Run
    // is missing, Apply must fail closed before reading or writing any file.
    ProposedChangeSet rawCs;
    rawCs.changeSetId = "cs-missing-run";
    rawCs.runId = "missing-run";
    rawCs.projectId = "p1";
    rawCs.state = ChangeSetState::ReadyForReview;
    f.repo.insertRaw(rawCs, {});

    auto applyService = f.makeApplyService();
    req(applyService.approve(rawCs.changeSetId).success(), "approve should succeed before Run resolution");
    const auto result = applyService.apply(rawCs.changeSetId);

    req(!result.success(), "apply should reject a proposal without its owning Run");
    req(result.error()->code == "proposal.run_not_found", "wrong error code");
}

void testApplyRejectsProjectIdentityMismatch()
{
    Fixture f;

    // Corrupted proposal metadata must not be allowed to borrow the valid
    // workspace snapshot of a Run that belongs to a different Project.
    ProposedChangeSet rawCs;
    rawCs.changeSetId = "cs-project-mismatch";
    rawCs.runId = "r1";
    rawCs.projectId = "different-project";
    rawCs.state = ChangeSetState::ReadyForReview;
    f.repo.insertRaw(rawCs, {});

    auto applyService = f.makeApplyService();
    req(applyService.approve(rawCs.changeSetId).success(), "approve should succeed before Run resolution");
    const auto result = applyService.apply(rawCs.changeSetId);

    req(!result.success(), "apply should reject mismatched Run/Project identity");
    req(result.error()->code == "proposal.project_mismatch", "wrong error code");
}

void testApplyUsesRunWorkspaceSnapshotAfterProjectPathChanges()
{
    Fixture f;
    TempDir redirectedProjectDir;

    auto proposalService = f.makeProposalService();
    CreateChangeProposalRequest request;
    request.runId = "r1";
    request.projectId = "p1";
    request.providerId = "prov1";
    request.modelId = "m1";
    request.agentRole = "test";
    request.rawModelOutput = makeCreateProposal("snapshot-target.txt", "snapshot");
    request.policy = f.defaultPolicy();

    const auto changeSet = proposalService.createFromModelOutput(std::move(request));
    req(changeSet.success(), "createFromModelOutput failed");
    req(changeSet.value().state == ChangeSetState::ReadyForReview,
        "expected ReadyForReview");

    // Simulate the user repointing the mutable Project after the Run completed.
    // Apply must remain attached to Run.workspacePath, not the new Project root.
    const auto projectResult = f.projects.findById("p1");
    req(projectResult.success() && projectResult.value().has_value(),
        "fixture project was not found");
    Project project = *projectResult.value();
    project.rootPath = fs::weakly_canonical(redirectedProjectDir.path);
    project.updatedAt = std::chrono::system_clock::now();
    req(f.projects.update(project).success(), "could not repoint fixture project");

    auto applyService = f.makeApplyService();
    req(applyService.approve(changeSet.value().changeSetId).success(), "approve failed");
    const auto result = applyService.apply(changeSet.value().changeSetId);

    req(result.success(), "apply against Run workspace snapshot failed");
    req(fs::exists(f.projectDir.path / "snapshot-target.txt"),
        "proposal was not applied to the Run workspace snapshot");
    req(!fs::exists(redirectedProjectDir.path / "snapshot-target.txt"),
        "proposal was incorrectly redirected to the Project's new root");
}

void testApplyCreate()
{
    Fixture f;
    auto proposalService = f.makeProposalService();
    auto applyService    = f.makeApplyService();

    CreateChangeProposalRequest req_;
    req_.runId      = "r1";
    req_.projectId  = "p1";
    req_.providerId = "prov1";
    req_.modelId    = "m1";
    req_.agentRole  = "test";
    req_.rawModelOutput = makeCreateProposal("subdir/newfile.txt", "hello world");
    req_.policy         = f.defaultPolicy();

    const auto cs = proposalService.createFromModelOutput(std::move(req_));
    req(cs.success(), "createFromModelOutput failed");
    req(cs.value().state == ChangeSetState::ReadyForReview, "expected ReadyForReview");

    const fs::path targetPath = f.projectDir.path / "subdir" / "newfile.txt";
    req(!fs::exists(targetPath), "file should not exist before apply");

    req(applyService.approve(cs.value().changeSetId).success(), "approve failed");
    const auto result = applyService.apply(cs.value().changeSetId);
    req(result.success(), "apply failed");
    req(result.value().appliedCount == 1, "wrong applied count");

    req(fs::exists(targetPath), "file should exist after apply");
    req(readFile(targetPath) == "hello world", "file content mismatch");

    // State should be Applied in the repository.
    const auto found = f.repo.findChangeSet(cs.value().changeSetId);
    req(found.success() && found.value().has_value(), "change set not found after apply");
    req(found.value()->state == ChangeSetState::Applied, "state should be Applied");

    // Apply audit trail: the approval row now carries the outcome and a
    // before/after hash for the one file created.
    const auto approval = f.approvals.findApproval(cs.value().changeSetId);
    req(approval.success() && approval.value().has_value(), "approval record missing");
    req(approval.value()->applyResult == "applied", "apply result not recorded");
    req(approval.value()->applyHashLog.size() == 1, "expected one hash-log entry");
    req(!approval.value()->applyHashLog[0].originalHash.has_value(),
        "Create must not have an original hash");
    req(approval.value()->applyHashLog[0].newHash.has_value(),
        "Create must record the written content's hash");
}

// Apply writes a file whose folder and name the ANSI code page of this machine cannot hold (Cyrillic on an English Windows, Chinese on
// a Russian one - the name has both): the manifest of the apply and the target path are built from the proposal's relative path, and
// path::generic_string() on such a name throws std::system_error.
void testApplyCreateWithAFileNameTheAnsiCodePageCannotHold()
{
    Fixture f;
    auto proposalService = f.makeProposalService();
    auto applyService    = f.makeApplyService();

    const std::string relative =
        reinterpret_cast<const char*>(u8"\u043f\u0430\u043f\u043a\u0430/\u6e2c\u8a66.txt");
    CreateChangeProposalRequest req_;
    req_.runId      = "r1";
    req_.projectId  = "p1";
    req_.providerId = "prov1";
    req_.modelId    = "m1";
    req_.agentRole  = "test";
    req_.rawModelOutput = makeCreateProposal(relative, "hello unicode");
    req_.policy         = f.defaultPolicy();

    const auto cs = proposalService.createFromModelOutput(std::move(req_));
    req(cs.success() && cs.value().state == ChangeSetState::ReadyForReview, "the proposal must be reviewable");
    const fs::path targetPath = f.projectDir.path / fs::path(std::u8string(
        reinterpret_cast<const char8_t*>(relative.data()), relative.size()));
    req(!fs::exists(targetPath), "file should not exist before apply");

    req(applyService.approve(cs.value().changeSetId).success(), "approve failed");
    const auto result = applyService.apply(cs.value().changeSetId);
    req(result.success(), ("apply failed: " + (result.success() ? std::string() : result.error()->code + " "
        + result.error()->message)).c_str());
    req(result.value().appliedCount == 1, "wrong applied count");
    req(fs::exists(targetPath) && readFile(targetPath) == "hello unicode", "the file must exist under its real name");
}

// PC-4C: a genuine approval nudges the Outcome Brief once (not the idempotent replay); EVERY apply does, whatever it ends as -
// applied, ApplyFailed or refused - because the outcome of the Run may have changed.
void testApproveAndApplyNudgeTheOutcomeBrief()
{
    Fixture f;
    auto proposalService = f.makeProposalService();
    auto applyService = f.makeApplyService();
    outcomebrief_hooks::RecordingTrigger trigger;
    applyService.setOutcomeBriefTrigger(&trigger);

    CreateChangeProposalRequest req_;
    req_.runId = "r1";
    req_.projectId = "p1";
    req_.providerId = "prov1";
    req_.modelId = "m1";
    req_.agentRole = "test";
    req_.rawModelOutput = makeCreateProposal("nudged.txt", "hello");
    req_.policy = f.defaultPolicy();
    const auto cs = proposalService.createFromModelOutput(std::move(req_));
    req(cs.success(), "createFromModelOutput failed");
    const std::string id = cs.value().changeSetId;

    req(!applyService.apply(id).success(), "an unapproved change set is refused");
    req(trigger.proposalCalls(id) == 1, "...and even a refused apply nudges (the outcome may have changed)");

    req(applyService.approve(id).success(), "approve failed");
    req(trigger.proposalCalls(id) == 2, "a genuine approval nudges once");
    req(applyService.approve(id).success(), "a repeated approval is not an error");
    req(trigger.proposalCalls(id) == 2, "...and the replay nudges nothing");

    req(applyService.apply(id).success(), "apply failed");
    req(trigger.proposalCalls(id) == 3 && trigger.runs().empty(), "an applied change set nudges once more");

    // An apply that fails (the file now exists) still nudges: the state became ApplyFailed.
    CreateChangeProposalRequest second;
    second.runId = "r1";
    second.projectId = "p1";
    second.providerId = "prov1";
    second.modelId = "m1";
    second.agentRole = "test";
    second.rawModelOutput = makeCreateProposal("conflict.txt", "first");
    second.policy = f.defaultPolicy();
    const auto conflict = proposalService.createFromModelOutput(std::move(second));
    req(conflict.success(), "createFromModelOutput failed for the conflicting proposal");
    req(applyService.approve(conflict.value().changeSetId).success(), "approve of the conflicting proposal failed");
    writeFile(f.projectDir.path / "conflict.txt", "somebody was faster");
    const auto failed = applyService.apply(conflict.value().changeSetId);
    req(!failed.success(), "applying onto an existing file must fail");
    req(trigger.proposalCalls(conflict.value().changeSetId) == 2, "a failed apply nudges too (approve + apply)");
}

void testApplyPersistsTruthfulPostApplyWorkspaceProvenance()
{
    Fixture f;
    writeFile(f.projectDir.path / "existing.txt", "baseline\n");
    const auto baseline = WorkspaceBaselineProvenance::capture(
        WorkspaceReadPolicy::secureDefaults(f.projectDir.path));
    req(baseline.success(), "baseline capture failed");
    const auto baselineJson = WorkspaceBaselineProvenance::renderJson(baseline.value());
    req(baselineJson.success(), "baseline rendering failed");
    req(f.artifactService.createArtifact(
            "r1", std::nullopt, ArtifactKind::ContextSnapshot,
            "workspace-baseline.v1.json", "application/json", baselineJson.value()).success(),
        "baseline artifact persistence failed");

    auto proposalService = f.makeProposalService();
    CreateChangeProposalRequest request;
    request.runId = "r1";
    request.projectId = "p1";
    request.providerId = "prov1";
    request.modelId = "m1";
    request.agentRole = "test";
    request.rawModelOutput = makeCreateProposal("created.txt", "created by proposal");
    request.policy = f.defaultPolicy();
    const auto proposal = proposalService.createFromModelOutput(std::move(request));
    req(proposal.success(), "proposal creation failed");

    auto applyService = f.makeApplyServiceWithArtifacts();
    req(applyService.approve(proposal.value().changeSetId).success(), "approve failed");
    const auto applied = applyService.apply(proposal.value().changeSetId);
    req(applied.success(), "apply failed");
    req(!applied.value().provenanceErrorCode,
        "post-apply provenance unexpectedly failed");
    const auto postApply = WorkspaceBaselineProvenance::capture(
        WorkspaceReadPolicy::secureDefaults(f.projectDir.path));
    const auto approval = f.approvals.findApproval(proposal.value().changeSetId);
    req(postApply.success() && approval.success() && approval.value().has_value()
            && approval.value()->postApplyWorkspaceFingerprint == postApply.value().fingerprint,
        "Approval audit must retain the authoritative post-Apply workspace fingerprint");

    const auto artifacts = f.artifactService.listArtifacts("r1");
    req(artifacts.success(), "artifact listing failed");
    const Artifact* report = nullptr;
    for (const auto& artifact : artifacts.value()) {
        if (artifact.name == "manifest-workspace-apply-provenance.md")
            report = &artifact;
    }
    req(report != nullptr, "post-apply provenance artifact missing");
    const auto content = f.artifactService.getArtifactContent(report->id);
    req(content.success(), "post-apply provenance artifact unreadable");
    req(content.value().find("Classification: applied_proposal_set") != std::string::npos,
        "proposal-only apply was not classified truthfully");
    req(content.value().find(proposal.value().changeSetId) != std::string::npos,
        "applied proposal lineage missing from provenance report");
}

void testApplyProvenanceDetectsUnrelatedWorkspaceDrift()
{
    Fixture f;
    const auto baseline = WorkspaceBaselineProvenance::capture(
        WorkspaceReadPolicy::secureDefaults(f.projectDir.path));
    req(baseline.success(), "baseline capture failed");
    const auto baselineJson = WorkspaceBaselineProvenance::renderJson(baseline.value());
    req(baselineJson.success(), "baseline rendering failed");
    req(f.artifactService.createArtifact(
            "r1", std::nullopt, ArtifactKind::ContextSnapshot,
            "workspace-baseline.v1.json", "application/json", baselineJson.value()).success(),
        "baseline artifact persistence failed");

    writeFile(f.projectDir.path / "manual.txt", "manual drift\n");
    auto proposalService = f.makeProposalService();
    CreateChangeProposalRequest request;
    request.runId = "r1";
    request.projectId = "p1";
    request.providerId = "prov1";
    request.modelId = "m1";
    request.agentRole = "test";
    request.rawModelOutput = makeCreateProposal("created.txt", "proposal");
    request.policy = f.defaultPolicy();
    const auto proposal = proposalService.createFromModelOutput(std::move(request));
    req(proposal.success(), "proposal creation failed");

    auto applyService = f.makeApplyServiceWithArtifacts();
    req(applyService.approve(proposal.value().changeSetId).success(), "approve failed");
    const auto applied = applyService.apply(proposal.value().changeSetId);
    req(applied.success() && !applied.value().provenanceErrorCode,
        "apply provenance failed");

    const auto artifacts = f.artifactService.listArtifacts("r1");
    const Artifact* report = nullptr;
    for (const auto& artifact : artifacts.value()) {
        if (artifact.name == "manifest-workspace-apply-provenance.md") report = &artifact;
    }
    req(report != nullptr, "post-apply provenance artifact missing");
    const auto content = f.artifactService.getArtifactContent(report->id);
    req(content.success()
            && content.value().find("Classification: applied_with_manual_or_external_drift")
                != std::string::npos
            && content.value().find("- manual.txt") != std::string::npos,
        "unrelated workspace drift was not identified by relative path");
}

void testApplyModify()
{
    Fixture f;

    const std::string original = "original content";
    const std::string proposed = "modified content";
    const fs::path targetPath  = f.projectDir.path / "file.txt";
    writeFile(targetPath, original);

    auto proposalService = f.makeProposalService();
    auto applyService    = f.makeApplyService();

    CreateChangeProposalRequest req_;
    req_.runId      = "r1";
    req_.projectId  = "p1";
    req_.providerId = "prov1";
    req_.modelId    = "m1";
    req_.agentRole  = "test";
    req_.rawModelOutput = makeModifyProposal("file.txt", original, proposed);
    req_.policy         = f.defaultPolicy();
    bindHashEvidence(req_, "file.txt", original);

    const auto cs = proposalService.createFromModelOutput(std::move(req_));
    req(cs.success(), "createFromModelOutput failed");
    req(cs.value().state == ChangeSetState::ReadyForReview, "expected ReadyForReview");

    req(applyService.approve(cs.value().changeSetId).success(), "approve failed");
    const auto result = applyService.apply(cs.value().changeSetId);
    req(result.success(), "apply failed");
    req(result.value().appliedCount == 1, "wrong applied count");
    req(readFile(targetPath) == proposed, "file content should be updated");

    const auto found = f.repo.findChangeSet(cs.value().changeSetId);
    req(found.value()->state == ChangeSetState::Applied, "state should be Applied");
}

void testApplyHashMismatch()
{
    Fixture f;

    const std::string original = "original content";
    const std::string proposed = "modified content";
    const fs::path targetPath  = f.projectDir.path / "file.txt";
    writeFile(targetPath, original);

    auto proposalService = f.makeProposalService();
    auto applyService    = f.makeApplyService();

    CreateChangeProposalRequest req_;
    req_.runId      = "r1";
    req_.projectId  = "p1";
    req_.providerId = "prov1";
    req_.modelId    = "m1";
    req_.agentRole  = "test";
    req_.rawModelOutput = makeModifyProposal("file.txt", original, proposed);
    req_.policy         = f.defaultPolicy();
    bindHashEvidence(req_, "file.txt", original);

    const auto cs = proposalService.createFromModelOutput(std::move(req_));
    req(cs.success(), "createFromModelOutput failed");
    req(applyService.approve(cs.value().changeSetId).success(), "approve failed");

    // Simulate a concurrent edit: change the file after proposal was staged.
    writeFile(targetPath, "someone else changed this");

    const auto result = applyService.apply(cs.value().changeSetId);
    req(!result.success(), "should fail on hash mismatch");
    req(result.error()->code == "proposal.hash_mismatch", "wrong error code");

    // Proposal state must remain ReadyForReview (no write attempted).
    const auto found = f.repo.findChangeSet(cs.value().changeSetId);
    req(found.value()->state == ChangeSetState::ReadyForReview,
        "state should stay ReadyForReview after hash mismatch");

    // A failed apply still records an audit outcome against the approval.
    const auto approval = f.approvals.findApproval(cs.value().changeSetId);
    req(approval.success() && approval.value().has_value(), "approval record missing");
    req(approval.value()->applyResult == "apply_failed", "apply_failed result not recorded");
    req(approval.value()->applyErrorCode.has_value()
            && *approval.value()->applyErrorCode == "proposal.hash_mismatch",
        "wrong recorded error code");
}

void testEvidenceBackedHashIsPersistedAndApplied()
{
    Fixture f;

    const std::string original = "original observed content";
    const std::string proposed = "trusted updated content";
    const std::string trustedHash = sha256Hex(original);
    const fs::path targetPath = f.projectDir.path / "observed.txt";
    writeFile(targetPath, original);

    auto proposalService = f.makeProposalService();
    auto applyService = f.makeApplyService();

    CreateChangeProposalRequest req_;
    req_.runId = "r1";
    req_.projectId = "p1";
    req_.providerId = "prov1";
    req_.modelId = "m1";
    req_.agentRole = "test";
    // proposal-submission.v2 has no hash field at all - the model cannot
    // send one, correct or otherwise; makeModifyProposal below never embeds
    // one, so the trusted hash bound just below is the only source of
    // evidence, exactly like AgentToolTurnLoopRunner::preflightTrustedHashes
    // in production.
    req_.rawModelOutput = makeModifyProposal("observed.txt", original, proposed);
    req_.policy = f.defaultPolicy();
    req_.requireHashEvidence = true;
    req_.observedFileHashes.emplace(
        detail::normalizedPathKey(fs::path("observed.txt")), trustedHash);

    const auto cs = proposalService.createFromModelOutput(std::move(req_));
    req(cs.success(), "evidence-backed proposal creation failed");
    req(cs.value().state == ChangeSetState::ReadyForReview,
        "evidence-backed proposal should be ReadyForReview");
    req(cs.value().changes.size() == 1, "expected one returned change");
    req(cs.value().changes.front().expectedOriginalHash == trustedHash,
        "returned change must carry the backend-observed hash");

    const auto persisted = f.repo.listChanges(cs.value().changeSetId);
    req(persisted.success() && persisted.value().size() == 1,
        "expected one persisted change");
    req(persisted.value().front().expectedOriginalHash == trustedHash,
        "persisted change must carry the backend-observed hash");

    req(applyService.approve(cs.value().changeSetId).success(), "approve failed");
    const auto result = applyService.apply(cs.value().changeSetId);
    req(result.success(), "unchanged evidence-backed proposal should apply");
    req(readFile(targetPath) == proposed,
        "evidence-backed apply must write the proposed content");
}

void testApplyWithoutApprovalFails()
{
    Fixture f;
    auto proposalService = f.makeProposalService();
    auto applyService    = f.makeApplyService();

    CreateChangeProposalRequest req_;
    req_.runId      = "r1";
    req_.projectId  = "p1";
    req_.providerId = "prov1";
    req_.modelId    = "m1";
    req_.agentRole  = "test";
    req_.rawModelOutput = makeCreateProposal("unapproved.txt", "hello");
    req_.policy         = f.defaultPolicy();

    const auto cs = proposalService.createFromModelOutput(std::move(req_));
    req(cs.success(), "createFromModelOutput failed");
    req(cs.value().state == ChangeSetState::ReadyForReview, "expected ReadyForReview");

    // No approve() call - apply() must refuse to run at all.
    const auto result = applyService.apply(cs.value().changeSetId);
    req(!result.success(), "apply without approval should fail");
    req(result.error()->code == "proposal.no_approval", "wrong error code");
    req(!fs::exists(f.projectDir.path / "unapproved.txt"),
        "no file should be written without approval");
}

void testApproveIsIdempotent()
{
    Fixture f;
    auto proposalService = f.makeProposalService();
    auto applyService    = f.makeApplyService();

    CreateChangeProposalRequest req_;
    req_.runId      = "r1";
    req_.projectId  = "p1";
    req_.providerId = "prov1";
    req_.modelId    = "m1";
    req_.agentRole  = "test";
    req_.rawModelOutput = makeCreateProposal("idempotent.txt", "hello");
    req_.policy         = f.defaultPolicy();

    const auto cs = proposalService.createFromModelOutput(std::move(req_));
    req(cs.success(), "createFromModelOutput failed");

    const auto first = applyService.approve(cs.value().changeSetId);
    req(first.success(), "first approve failed");
    const auto second = applyService.approve(cs.value().changeSetId);
    req(second.success(), "re-approving the same snapshot must not fail");
    req(second.value().snapshotHash == first.value().snapshotHash,
        "idempotent approve returned a different snapshot hash");
    req(second.value().approvedAt == first.value().approvedAt,
        "idempotent approve must return the original record, not a new one");
}

void testApplyWithMismatchedApprovalFails()
{
    Fixture f;
    auto proposalService = f.makeProposalService();
    auto applyService    = f.makeApplyService();

    CreateChangeProposalRequest req_;
    req_.runId      = "r1";
    req_.projectId  = "p1";
    req_.providerId = "prov1";
    req_.modelId    = "m1";
    req_.agentRole  = "test";
    req_.rawModelOutput = makeCreateProposal("mismatched.txt", "hello");
    req_.policy         = f.defaultPolicy();

    const auto cs = proposalService.createFromModelOutput(std::move(req_));
    req(cs.success(), "createFromModelOutput failed");
    req(applyService.approve(cs.value().changeSetId).success(), "approve failed");

    // Corrupt the stored approval's hash without going through the normal
    // (deliberately immutable) path - simulates a stale/tampered record.
    f.approvals.overwriteSnapshotHashForTest(cs.value().changeSetId, "not-the-real-hash");

    const auto result = applyService.apply(cs.value().changeSetId);
    req(!result.success(), "apply with a mismatched approval should fail");
    req(result.error()->code == "proposal.approval_mismatch", "wrong error code");
    req(!fs::exists(f.projectDir.path / "mismatched.txt"),
        "no file should be written when the approval does not match");
}

void testApplyCreateConflict()
{
    Fixture f;

    const fs::path targetPath = f.projectDir.path / "existing.txt";
    writeFile(targetPath, "already here");

    auto proposalService = f.makeProposalService();
    auto applyService    = f.makeApplyService();

    CreateChangeProposalRequest req_;
    req_.runId      = "r1";
    req_.projectId  = "p1";
    req_.providerId = "prov1";
    req_.modelId    = "m1";
    req_.agentRole  = "test";
    req_.rawModelOutput = makeCreateProposal("existing.txt", "new content");
    req_.policy         = f.defaultPolicy();

    // createFromModelOutput won't catch this — it only checks hashes for
    // Modify/Delete; Create just verifies structural validity.  The apply
    // service catches it at apply time via the pre-flight conflict check.
    const auto cs = proposalService.createFromModelOutput(std::move(req_));
    // The proposal may succeed (target existence isn't pre-checked by validator
    // for Create) OR be ReadyForReview; either way we exercise the apply path.
    if (!cs.success()) return; // validator caught it; nothing left to test
    if (cs.value().state != ChangeSetState::ReadyForReview) return;

    req(applyService.approve(cs.value().changeSetId).success(), "approve failed");
    const auto result = applyService.apply(cs.value().changeSetId);
    req(!result.success(), "should fail when create target already exists");
    req(result.error()->code == "proposal.create_conflict", "wrong error code");
    // Original file must be untouched.
    req(readFile(targetPath) == "already here", "original file should be unchanged");
}

void testApplyDelete()
{
    Fixture f;

    const std::string original = "delete me";
    const fs::path targetPath = f.projectDir.path / "todelete.txt";
    writeFile(targetPath, original);

    auto proposalService = f.makeProposalService();
    auto applyService    = f.makeApplyService();

    CreateChangeProposalRequest req_;
    req_.runId      = "r1";
    req_.projectId  = "p1";
    req_.providerId = "prov1";
    req_.modelId    = "m1";
    req_.agentRole  = "test";
    req_.rawModelOutput = makeDeleteProposal("todelete.txt", original);
    req_.policy         = f.defaultPolicy();
    req_.policy.allowDelete = true;
    bindHashEvidence(req_, "todelete.txt", original);

    const auto cs = proposalService.createFromModelOutput(std::move(req_));
    req(cs.success(), "createFromModelOutput failed");
    req(cs.value().state == ChangeSetState::ReadyForReview, "expected ReadyForReview");

    req(applyService.approve(cs.value().changeSetId).success(), "approve failed");
    const auto result = applyService.apply(cs.value().changeSetId);
    req(result.success(), "apply failed");
    req(result.value().appliedCount == 1, "wrong applied count");
    req(!fs::exists(targetPath), "file should not exist after delete");

    const auto found = f.repo.findChangeSet(cs.value().changeSetId);
    req(found.value()->state == ChangeSetState::Applied, "state should be Applied");

    const auto approval = f.approvals.findApproval(cs.value().changeSetId);
    req(approval.success() && approval.value().has_value(), "approval record missing");
    req(approval.value()->applyHashLog.size() == 1, "expected one hash-log entry");
    req(approval.value()->applyHashLog[0].originalHash.has_value(),
        "Delete must record the pre-deletion hash");
    req(!approval.value()->applyHashLog[0].newHash.has_value(),
        "Delete must not have a new-content hash");
}

void testApplyDeleteMissingTarget()
{
    Fixture f;
    auto proposalService = f.makeProposalService();
    auto applyService    = f.makeApplyService();

    CreateChangeProposalRequest req_;
    req_.runId      = "r1";
    req_.projectId  = "p1";
    req_.providerId = "prov1";
    req_.modelId    = "m1";
    req_.agentRole  = "test";
    req_.rawModelOutput = makeDeleteProposal("missing.txt", "anything");
    req_.policy         = f.defaultPolicy();
    req_.policy.allowDelete = true;

    const auto cs = proposalService.createFromModelOutput(std::move(req_));
    req(cs.success(), "createFromModelOutput failed");
    // The validator only checks structural invariants, not filesystem
    // existence, so this reaches ReadyForReview and apply()'s pre-flight
    // catches the missing target.
    if (cs.value().state != ChangeSetState::ReadyForReview) return;

    req(applyService.approve(cs.value().changeSetId).success(), "approve failed");
    const auto result = applyService.apply(cs.value().changeSetId);
    req(!result.success(), "should fail when delete target is missing");
    req(result.error()->code == "proposal.target_missing", "wrong error code");
}

void testApplyRename()
{
    Fixture f;

    const std::string original = "rename me";
    const fs::path sourcePath = f.projectDir.path / "old.txt";
    const fs::path targetPath = f.projectDir.path / "new.txt";
    writeFile(sourcePath, original);

    auto proposalService = f.makeProposalService();
    auto applyService    = f.makeApplyService();

    CreateChangeProposalRequest req_;
    req_.runId      = "r1";
    req_.projectId  = "p1";
    req_.providerId = "prov1";
    req_.modelId    = "m1";
    req_.agentRole  = "test";
    req_.rawModelOutput = makeRenameProposal("old.txt", "new.txt", original);
    req_.policy         = f.defaultPolicy();
    req_.policy.allowRename = true;
    bindHashEvidence(req_, "old.txt", original);

    const auto cs = proposalService.createFromModelOutput(std::move(req_));
    req(cs.success(), "createFromModelOutput failed");
    req(cs.value().state == ChangeSetState::ReadyForReview, "expected ReadyForReview");

    req(applyService.approve(cs.value().changeSetId).success(), "approve failed");
    const auto result = applyService.apply(cs.value().changeSetId);
    req(result.success(), "apply failed");
    req(result.value().appliedCount == 1, "wrong applied count");
    req(!fs::exists(sourcePath), "source file should no longer exist");
    req(fs::exists(targetPath), "target file should exist after rename");
    req(readFile(targetPath) == original, "renamed file content mismatch");

    const auto found = f.repo.findChangeSet(cs.value().changeSetId);
    req(found.value()->state == ChangeSetState::Applied, "state should be Applied");

    const auto approval = f.approvals.findApproval(cs.value().changeSetId);
    req(approval.success() && approval.value().has_value(), "approval record missing");
    req(approval.value()->applyHashLog.size() == 1, "expected one hash-log entry");
    req(approval.value()->applyHashLog[0].originalHash.has_value()
            && approval.value()->applyHashLog[0].newHash.has_value()
            && *approval.value()->applyHashLog[0].originalHash
                   == *approval.value()->applyHashLog[0].newHash,
        "Rename must record the unchanged content hash on both sides");
}

void testApplyRenameTargetConflict()
{
    Fixture f;

    const std::string original = "rename me";
    const fs::path sourcePath = f.projectDir.path / "old.txt";
    const fs::path targetPath = f.projectDir.path / "new.txt";
    writeFile(sourcePath, original);
    writeFile(targetPath, "already occupies the target name");

    auto proposalService = f.makeProposalService();
    auto applyService    = f.makeApplyService();

    CreateChangeProposalRequest req_;
    req_.runId      = "r1";
    req_.projectId  = "p1";
    req_.providerId = "prov1";
    req_.modelId    = "m1";
    req_.agentRole  = "test";
    req_.rawModelOutput = makeRenameProposal("old.txt", "new.txt", original);
    req_.policy         = f.defaultPolicy();
    req_.policy.allowRename = true;
    bindHashEvidence(req_, "old.txt", original);

    const auto cs = proposalService.createFromModelOutput(std::move(req_));
    if (!cs.success()) return;
    if (cs.value().state != ChangeSetState::ReadyForReview) return;

    req(applyService.approve(cs.value().changeSetId).success(), "approve failed");
    const auto result = applyService.apply(cs.value().changeSetId);
    req(!result.success(), "should fail when rename target already exists");
    req(result.error()->code == "proposal.rename_target_conflict", "wrong error code");
    req(fs::exists(sourcePath), "source file must remain untouched");
    req(readFile(targetPath) == "already occupies the target name",
        "existing target file must be unchanged");
}

void testApplyRenameRollbackOnLaterFailure()
{
    // Two-change proposal: Rename succeeds first (no store dependency), then
    // a Create fails because the store has no published artifact for it
    // (artifactRelativeRoot left empty, same technique as
    // testApplyRollbackOnWriteFailure). The already-applied rename must be
    // rolled back.
    Fixture f;

    const std::string original = "rename me";
    const fs::path sourcePath = f.projectDir.path / "old.txt";
    const fs::path targetPath = f.projectDir.path / "new.txt";
    writeFile(sourcePath, original);

    ProposedChangeSet rawCs;
    rawCs.changeSetId         = "cs-rename-rollback";
    rawCs.runId               = "r1";
    rawCs.projectId           = "p1";
    rawCs.providerId          = "prov1";
    rawCs.modelId             = "m1";
    rawCs.agentRole           = "test";
    rawCs.summary             = "rename rollback test";
    rawCs.baseRevision        = "";
    rawCs.state               = ChangeSetState::ReadyForReview;
    rawCs.artifactRelativeRoot = ""; // empty → store_.readProposedContent will fail

    ProposedFileChange renameChange;
    renameChange.changeId      = "rn1";
    renameChange.operation     = FileChangeOperation::Rename;
    renameChange.relativePath  = "new.txt";
    renameChange.sourcePath    = fs::path("old.txt");
    renameChange.expectedOriginalHash = sha256Hex(original);

    ProposedFileChange createChange;
    createChange.changeId     = "new-change-1";
    createChange.operation    = FileChangeOperation::Create;
    createChange.relativePath = "brand-new-file.txt";
    createChange.proposedContent = "some content";

    f.repo.insertRaw(rawCs, {renameChange, createChange});

    auto applyService = f.makeApplyService();
    req(applyService.approve("cs-rename-rollback").success(), "approve failed");
    const auto result = applyService.apply("cs-rename-rollback");

    req(!result.success(), "apply should fail when store has no artifacts for the create");
    req(fs::exists(sourcePath), "renamed-away source should be restored after rollback");
    req(!fs::exists(targetPath), "rename target should not exist after rollback");
    req(readFile(sourcePath) == original, "restored source content should be unchanged");
    req(!fs::exists(f.projectDir.path / "brand-new-file.txt"),
        "the failed create must not have written anything");
}

void testApplyRenameCaseOnly()
{
    // Regression test: a case-only rename target (e.g. "Main.qml" when only
    // "main.qml" currently exists) must not be flagged as an existing-file
    // conflict on a case-insensitive/case-preserving filesystem (NTFS on
    // Windows), and applying it must actually flip the on-disk casing
    // rather than silently no-op because the OS resolves source and target
    // to the same directory entry.
    Fixture f;

    const std::string original = "case-only rename content";
    writeFile(f.projectDir.path / "main.qml", original);

    auto proposalService = f.makeProposalService();
    auto applyService    = f.makeApplyService();

    CreateChangeProposalRequest req_;
    req_.runId      = "r1";
    req_.projectId  = "p1";
    req_.providerId = "prov1";
    req_.modelId    = "m1";
    req_.agentRole  = "test";
    req_.rawModelOutput = makeRenameProposal("main.qml", "Main.qml", original);
    req_.policy         = f.defaultPolicy();
    req_.policy.allowRename = true;
    bindHashEvidence(req_, "main.qml", original);

    const auto cs = proposalService.createFromModelOutput(std::move(req_));
    req(cs.success(), "createFromModelOutput failed");
    req(cs.value().state == ChangeSetState::ReadyForReview,
        "case-only rename target must not be treated as an existing-file conflict");

    req(applyService.approve(cs.value().changeSetId).success(), "approve failed");
    const auto result = applyService.apply(cs.value().changeSetId);
    req(result.success(), "apply failed");
    req(result.value().appliedCount == 1, "wrong applied count");

    req(readFile(f.projectDir.path / "Main.qml") == original,
        "renamed file content must be unchanged");
    req(actualFilenameCasing(f.projectDir.path, "Main.qml") == "Main.qml",
        "on-disk directory entry must actually carry the new casing");
    req(!hasLeftoverRenameTempFile(f.projectDir.path),
        "no temporary rename file should remain after a successful apply");
}

void testApplyRenameCaseOnlyRollbackRestoresOriginalCasing()
{
    // Same setup as testApplyRenameRollbackOnLaterFailure, but the succeeding
    // first change is a case-only rename - rollback must restore the exact
    // original on-disk casing, not leave it renamed or stuck on a temp name.
    Fixture f;

    const std::string original = "case-only rollback content";
    writeFile(f.projectDir.path / "main.qml", original);

    ProposedChangeSet rawCs;
    rawCs.changeSetId         = "cs-case-rename-rollback";
    rawCs.runId               = "r1";
    rawCs.projectId           = "p1";
    rawCs.providerId          = "prov1";
    rawCs.modelId             = "m1";
    rawCs.agentRole           = "test";
    rawCs.summary             = "case-only rename rollback test";
    rawCs.baseRevision        = "";
    rawCs.state               = ChangeSetState::ReadyForReview;
    rawCs.artifactRelativeRoot = ""; // empty → store_.readProposedContent will fail

    ProposedFileChange renameChange;
    renameChange.changeId      = "rn1";
    renameChange.operation     = FileChangeOperation::Rename;
    renameChange.relativePath  = "Main.qml";
    renameChange.sourcePath    = fs::path("main.qml");
    renameChange.expectedOriginalHash = sha256Hex(original);

    ProposedFileChange createChange;
    createChange.changeId     = "new-change-1";
    createChange.operation    = FileChangeOperation::Create;
    createChange.relativePath = "brand-new-file.txt";
    createChange.proposedContent = "some content";

    f.repo.insertRaw(rawCs, {renameChange, createChange});

    auto applyService = f.makeApplyService();
    req(applyService.approve("cs-case-rename-rollback").success(), "approve failed");
    const auto result = applyService.apply("cs-case-rename-rollback");

    req(!result.success(), "apply should fail when store has no artifacts for the create");
    req(readFile(f.projectDir.path / "main.qml") == original,
        "restored source content should be unchanged");
    req(actualFilenameCasing(f.projectDir.path, "main.qml") == "main.qml",
        "rollback must restore the original casing exactly");
    req(!hasLeftoverRenameTempFile(f.projectDir.path),
        "no temporary rename file should remain after rollback");
}

void testApplyRollbackOnWriteFailure()
{
    // Two-change proposal: first Create succeeds, second Modify fails due
    // to hash mismatch on disk.  The first change should be rolled back.
    Fixture f;

    const std::string original1 = "content of file1";
    const fs::path file1 = f.projectDir.path / "file1.txt";
    writeFile(file1, original1);

    // Build a proposal with two changes by using insertRaw to bypass
    // the single-entry JSON limit (the parser handles arrays, but building
    // two-change JSON by hand is cleaner for this edge-case test).
    ProposedChangeSet rawCs;
    rawCs.changeSetId         = "cs-rollback-test";
    rawCs.runId               = "r1";
    rawCs.projectId           = "p1";
    rawCs.providerId          = "prov1";
    rawCs.modelId             = "m1";
    rawCs.agentRole           = "test";
    rawCs.summary             = "rollback test";
    rawCs.baseRevision        = "";
    rawCs.state               = ChangeSetState::ReadyForReview;
    rawCs.artifactRelativeRoot = ""; // empty → store_.readProposedContent will fail

    // The store doesn't know about these changes (no published artifacts),
    // so readProposedContent will fail for both, triggering the rollback path.
    // We inject the changes directly into the repository to simulate a
    // partially-published state that fails mid-apply.

    // For a Create: store lookup will fail → triggers rollback after 0 ops done.
    ProposedFileChange change;
    change.changeId   = "new-change-1";
    change.operation  = FileChangeOperation::Create;
    change.relativePath = "brand-new-file.txt";
    change.proposedContent = "some content";

    f.repo.insertRaw(rawCs, {change});

    auto applyService = f.makeApplyService();
    req(applyService.approve("cs-rollback-test").success(), "approve failed");
    const auto result = applyService.apply("cs-rollback-test");

    // The apply must fail (store has no artifacts for this changeSetId).
    req(!result.success(), "apply should fail when store has no artifacts");
    // The target file must NOT have been created.
    req(!fs::exists(f.projectDir.path / "brand-new-file.txt"),
        "target file should not exist after rollback");
}

} // namespace

int runChangeSetApplyServiceTests()
{
    testApplyNotFound();
    testApplyNotReady();
    testApplyRejectsMissingOwningRun();
    testApplyRejectsProjectIdentityMismatch();
    testApplyUsesRunWorkspaceSnapshotAfterProjectPathChanges();
    testApplyCreate();
    testApplyCreateWithAFileNameTheAnsiCodePageCannotHold();
    testApproveAndApplyNudgeTheOutcomeBrief();
    testApplyPersistsTruthfulPostApplyWorkspaceProvenance();
    testApplyProvenanceDetectsUnrelatedWorkspaceDrift();
    testApplyModify();
    testApplyHashMismatch();
    testEvidenceBackedHashIsPersistedAndApplied();
    testApplyCreateConflict();
    testApplyDelete();
    testApplyDeleteMissingTarget();
    testApplyRename();
    testApplyRenameTargetConflict();
    testApplyRenameRollbackOnLaterFailure();
    testApplyRenameCaseOnly();
    testApplyRenameCaseOnlyRollbackRestoresOriginalCasing();
    testApplyRollbackOnWriteFailure();
    testApplyWithoutApprovalFails();
    testApproveIsIdempotent();
    testApplyWithMismatchedApprovalFails();
    return 0;
}
