#include "ai/application/proposals/ChangeProposalService.hpp"
#include "ai/application/proposals/ChangeProposalValidator.hpp"
#include "ai/application/workspace/FilesystemPathUtils.hpp"
#include "ai/application/proposals/ProposalReviewService.hpp"
#include "RecordingOutcomeBriefTrigger.hpp"
#include "ai/application/proposals/ProposedChangeSetParser.hpp"
#include "ai/application/workspace/ProjectWritePolicy.hpp"
#include "ai/application/execution/RunPathClaimRegistry.hpp"
#include "ai/application/Sha256.hpp"
#include "ai/infrastructure/FileChangeProposalStore.hpp"
#include "ai/infrastructure/SqliteRepositories.hpp"

// Native SQLite only, for the batched-read fault-injection test below - see
// the identical pattern in SqliteRepositoryTests.cpp.
#include <sqlite3.h>

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

namespace fs = std::filesystem;
namespace common = ai::common;
using namespace ai::application;
using namespace ai::infrastructure;

// ---------------------------------------------------------------------------
// Infrastructure helpers
// ---------------------------------------------------------------------------

struct TempDir {
    fs::path path;

    TempDir()
    {
        path = fs::temp_directory_path()
            / ("ai-orchestrator-proposal-test-"
                + std::to_string(
                    std::chrono::steady_clock::now().time_since_epoch().count()));
        fs::create_directories(path);
    }

    ~TempDir()
    {
        std::error_code ec;
        fs::remove_all(path, ec);
    }

    void writeFile(const std::string& rel, const std::string& content) const
    {
        const fs::path target = path / rel;
        fs::create_directories(target.parent_path());
        std::ofstream out(target, std::ios::binary | std::ios::trunc);
        out.write(content.data(), static_cast<std::streamsize>(content.size()));
    }
};

// Minimal in-memory IChangeProposalRepository for service tests.
class FakeChangeProposalRepository final : public IChangeProposalRepository {
public:
    common::Result<void> insertChangeSet(const ProposedChangeSet& cs) override
    {
        std::lock_guard lock(mu_);
        if (byId_.count(cs.changeSetId)) {
            return common::Result<void>::failure(
                {"proposal.duplicate_id", "Duplicate changeSetId"});
        }
        byId_[cs.changeSetId] = cs;
        byId_[cs.changeSetId].changes.clear(); // header only like SQLite
        byId_[cs.changeSetId].validationFindings.clear();
        changeLists_[cs.changeSetId] = cs.changes;
        findingLists_[cs.changeSetId] = cs.validationFindings;
        return common::Result<void>::ok();
    }

    common::Result<ProposedChangeSet> updateChangeSetState(
        const ChangeSetId& id, ChangeSetState newState) override
    {
        std::lock_guard lock(mu_);
        const auto it = byId_.find(id);
        if (it == byId_.end()) {
            return common::Result<ProposedChangeSet>::failure(
                {"proposal.not_found", "Change set not found"});
        }
        it->second.state = newState;
        return common::Result<ProposedChangeSet>::ok(it->second);
    }

    common::Result<void> markMissingEvidence(
        const ChangeSetId& id, std::string code, std::string message) override
    {
        std::lock_guard lock(mu_);
        const auto it = byId_.find(id);
        if (it == byId_.end())
            return common::Result<void>::failure({"proposal.not_found", "Change set not found"});
        it->second.missingEvidenceCode = std::move(code);
        it->second.missingEvidenceMessage = std::move(message);
        return common::Result<void>::ok();
    }

    common::Result<std::optional<ProposedChangeSet>> findChangeSet(
        const ChangeSetId& id) const override
    {
        std::lock_guard lock(mu_);
        const auto it = byId_.find(id);
        if (it == byId_.end()) return common::Result<std::optional<ProposedChangeSet>>::ok(std::nullopt);
        return common::Result<std::optional<ProposedChangeSet>>::ok(it->second);
    }

    common::Result<std::vector<ProposedChangeSet>> listForRun(const RunId& runId) const override
    {
        std::lock_guard lock(mu_);
        std::vector<ProposedChangeSet> result;
        for (const auto& [id, cs] : byId_) {
            if (cs.runId == runId) result.push_back(cs);
        }
        return common::Result<std::vector<ProposedChangeSet>>::ok(std::move(result));
    }

    common::Result<std::vector<ProposedChangeSet>> listForProject(
        const ProjectId& projectId) const override
    {
        std::lock_guard lock(mu_);
        std::vector<ProposedChangeSet> result;
        for (const auto& [id, cs] : byId_) {
            if (cs.projectId == projectId) result.push_back(cs);
        }
        return common::Result<std::vector<ProposedChangeSet>>::ok(std::move(result));
    }

    common::Result<std::vector<ProposedFileChange>> listChanges(
        const ChangeSetId& csId) const override
    {
        std::lock_guard lock(mu_);
        const auto it = changeLists_.find(csId);
        if (it == changeLists_.end())
            return common::Result<std::vector<ProposedFileChange>>::ok({});
        return common::Result<std::vector<ProposedFileChange>>::ok(it->second);
    }

    common::Result<std::vector<ValidationFinding>> listValidationFindings(
        const ChangeSetId& csId) const override
    {
        std::lock_guard lock(mu_);
        const auto it = findingLists_.find(csId);
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

    std::size_t size() const
    {
        std::lock_guard lock(mu_);
        return byId_.size();
    }

private:
    mutable std::mutex mu_;
    std::map<ChangeSetId, ProposedChangeSet> byId_;
    std::map<ChangeSetId, std::vector<ProposedFileChange>> changeLists_;
    std::map<ChangeSetId, std::vector<ValidationFinding>> findingLists_;
};

// Minimal in-memory IProposalReviewRepository for ProposalReviewService
// (AG-07) tests - same style as FakeChangeProposalRepository above.
class FakeProposalReviewRepository final : public IProposalReviewRepository {
public:
    common::Result<void> recordRejection(const ProposalReviewRejection& record) override
    {
        std::lock_guard lock(mu_);
        if (byId_.count(record.changeSetId)) {
            return common::Result<void>::failure(
                {"proposal.duplicate_id", "Change set already has a reviewer rejection record"});
        }
        byId_[record.changeSetId] = record;
        return common::Result<void>::ok();
    }

    common::Result<std::optional<ProposalReviewRejection>> findRejection(
        const ChangeSetId& changeSetId) const override
    {
        std::lock_guard lock(mu_);
        const auto it = byId_.find(changeSetId);
        if (it == byId_.end())
            return common::Result<std::optional<ProposalReviewRejection>>::ok(std::nullopt);
        return common::Result<std::optional<ProposalReviewRejection>>::ok(it->second);
    }

private:
    mutable std::mutex mu_;
    std::map<ChangeSetId, ProposalReviewRejection> byId_;
};

TrustedChangeSetMetadata makeMeta(
    const std::string& runId      = "run-001",
    const std::string& projectId  = "proj-001",
    const std::string& providerId = "openai",
    const std::string& modelId    = "gpt-4")
{
    TrustedChangeSetMetadata m;
    m.runId      = runId;
    m.projectId  = projectId;
    m.providerId = providerId;
    m.modelId    = modelId;
    m.agentRole  = "code-agent";
    m.baseRevision = "main";
    return m;
}

// ---------------------------------------------------------------------------
// Parser tests
// ---------------------------------------------------------------------------

void testParseValidCreateRawJson()
{
    const char* raw = R"({
        "schemaVersion": "proposal-submission.v2",
        "summary": "Add hello world",
        "changes": [{
            "change_id": "chg-001",
            "operation": "create",
            "relative_path": "src/hello.cpp",
            "source_path": null,
            "proposed_content": "#include <iostream>"
        }]
    })";

    const auto result = ProposedChangeSetParser::parse(raw, makeMeta());
    assert(result.success());
    const auto& cs = result.value();
    assert(cs.schemaVersion == "proposal.v1");
    assert(cs.runId == "run-001");
    assert(cs.projectId == "proj-001");
    assert(cs.state == ChangeSetState::Received);
    assert(!cs.changeSetId.empty());
    assert(cs.summary == "Add hello world");
    assert(cs.changes.size() == 1);
    assert(cs.changes[0].changeId == "chg-001");
    assert(cs.changes[0].operation == FileChangeOperation::Create);
    assert(cs.changes[0].relativePath == fs::path("src/hello.cpp"));
    assert(cs.changes[0].proposedContent.has_value());
    assert(*cs.changes[0].proposedContent == "#include <iostream>");
    assert(!cs.changes[0].expectedOriginalHash.has_value());
}

// Regression test: std::filesystem::path's std::string constructor has no
// portable narrow encoding across platforms - on Windows it interprets the
// bytes using the current system ANSI codepage, not UTF-8. relative_path/
// source_path are UTF-8 text straight from the model's JSON output; a
// non-ASCII path used to be silently corrupted (a Cyrillic "Тест/файл.txt"
// proposal ended up written to disk as mojibake, e.g. "РўРµСЃС‚/..."). The
// u8R"()" literal below guarantees UTF-8 bytes in the test regardless of
// the compiler's assumed source-file encoding.
void testParseNonAsciiRelativePathPreservesUtf8()
{
    const std::u8string rawU8 = u8R"({
        "schemaVersion": "proposal-submission.v2",
        "summary": "Non-ASCII path",
        "changes": [{
            "change_id": "chg-utf8",
            "operation": "create",
            "relative_path": "Тест/файл.txt",
            "source_path": null,
            "proposed_content": "hello"
        }]
    })";
    const std::string raw(reinterpret_cast<const char*>(rawU8.data()), rawU8.size());

    const auto result = ProposedChangeSetParser::parse(raw, makeMeta());
    assert(result.success());
    const std::u8string expectedPath = u8"Тест/файл.txt";
    assert(result.value().changes[0].relativePath.generic_u8string() == expectedPath);
}

void testParseValidCreateFenced()
{
    const char* raw = R"(```json
{
    "schemaVersion": "proposal-submission.v2",
    "summary": "Fenced proposal",
    "changes": [{
        "change_id": "chg-f1",
        "operation": "create",
        "relative_path": "README.md",
        "source_path": null,
        "proposed_content": "# Hello"
    }]
}
```)";

    const auto result = ProposedChangeSetParser::parse(raw, makeMeta());
    assert(result.success());
    assert(result.value().summary == "Fenced proposal");
    assert(result.value().changes[0].changeId == "chg-f1");
}

// The provider-facing contract has no expected_original_hash field at all
// (proposal-submission.v2): the parser never reads or sets one from model
// output, regardless of operation. The Backend's own preflight
// (AgentToolTurnLoopRunner::preflightTrustedHashes) is what populates it,
// which this parser-only test never runs - so expectedOriginalHash is
// unconditionally absent here even for a Modify/Delete/Rename.
void testParseValidModify()
{
    const char* raw = R"({
        "schemaVersion": "proposal-submission.v2",
        "summary": "Modify file",
        "changes": [{
            "change_id": "chg-m1",
            "operation": "modify",
            "relative_path": "src/foo.cpp",
            "source_path": null,
            "proposed_content": "new content"
        }]
    })";

    const auto result = ProposedChangeSetParser::parse(raw, makeMeta());
    assert(result.success());
    const auto& ch = result.value().changes[0];
    assert(ch.operation == FileChangeOperation::Modify);
    assert(!ch.expectedOriginalHash.has_value());
    assert(ch.proposedContent.has_value());
}

void testParseValidDelete()
{
    const char* raw = R"({
        "schemaVersion": "proposal-submission.v2",
        "summary": "Delete file",
        "changes": [{
            "change_id": "chg-d1",
            "operation": "delete",
            "relative_path": "src/old.cpp",
            "source_path": null,
            "proposed_content": null
        }]
    })";

    const auto result = ProposedChangeSetParser::parse(raw, makeMeta());
    assert(result.success());
    const auto& ch = result.value().changes[0];
    assert(ch.operation == FileChangeOperation::Delete);
    assert(!ch.proposedContent.has_value());
    assert(!ch.expectedOriginalHash.has_value());
}

void testParseValidRename()
{
    const char* raw = R"({
        "schemaVersion": "proposal-submission.v2",
        "summary": "Rename file",
        "changes": [{
            "change_id": "chg-r1",
            "operation": "rename",
            "relative_path": "src/new.cpp",
            "source_path": "src/old.cpp",
            "proposed_content": null
        }]
    })";

    const auto result = ProposedChangeSetParser::parse(raw, makeMeta());
    assert(result.success());
    const auto& ch = result.value().changes[0];
    assert(ch.operation == FileChangeOperation::Rename);
    assert(ch.sourcePath.has_value());
    assert(*ch.sourcePath == fs::path("src/old.cpp"));
    assert(!ch.proposedContent.has_value());
    assert(!ch.expectedOriginalHash.has_value());
}

void testParseEmptyChangesArray()
{
    const char* raw =
        R"({"schemaVersion":"proposal-submission.v2","summary":"No changes","changes":[]})";
    const auto result = ProposedChangeSetParser::parse(raw, makeMeta());
    assert(result.success());
    assert(result.value().changes.empty());
}

void testParseMalformedJson()
{
    const auto result = ProposedChangeSetParser::parse("{ not valid json", makeMeta());
    assert(!result.success());
    assert(result.error()->code == "proposal.parse_error");
}

void testParseExtraContentBeforeFence()
{
    const char* raw = "Some explanation\n```json\n"
        "{\"schemaVersion\":\"proposal-submission.v2\",\"summary\":\"s\",\"changes\":[]}\n```";
    const auto result = ProposedChangeSetParser::parse(raw, makeMeta());
    assert(!result.success());
    assert(result.error()->code == "proposal.extra_content");
}

void testParseExtraContentAfterFence()
{
    const char* raw = "```json\n"
        "{\"schemaVersion\":\"proposal-submission.v2\",\"summary\":\"s\",\"changes\":[]}\n"
        "```\nExtra text";
    const auto result = ProposedChangeSetParser::parse(raw, makeMeta());
    assert(!result.success());
    assert(result.error()->code == "proposal.extra_content");
}

void testParseMultipleFences()
{
    const char* raw = "```json\n{\"a\":1}\n```\n```json\n{\"b\":2}\n```";
    const auto result = ProposedChangeSetParser::parse(raw, makeMeta());
    assert(!result.success());
    assert(result.error()->code == "proposal.multiple_fences");
}

void testParseFenceContentContainingEmbeddedBacktickMention()
{
    // Regression test: the model's own JSON string value legitimately
    // contains a literal ``` substring (e.g. an acknowledgement mentioning
    // "the ```json fence"). The closing-fence search must not stop at that
    // inline occurrence — only a ``` that starts its own line closes the
    // fence. Previously this truncated the JSON mid-string and produced a
    // spurious "not valid JSON" failure.
    const char* raw = "```json\n"
        "{\"schemaVersion\": \"proposal-submission.v2\", "
        "\"summary\": \"I will use a ```json fence next time\", \"changes\": []}\n"
        "```";
    const auto result = ProposedChangeSetParser::parse(raw, makeMeta());
    assert(result.success());
    assert(result.value().summary == "I will use a ```json fence next time");
    assert(result.value().changes.empty());
}

void testParseMissingSummary()
{
    const auto result = ProposedChangeSetParser::parse(
        R"({"schemaVersion":"proposal-submission.v2","changes":[]})", makeMeta());
    assert(!result.success());
    // missing_field or empty_summary
    assert(result.error()->code == "proposal.missing_field"
        || result.error()->code == "proposal.empty_summary");
}

void testParseMissingChangesArray()
{
    const auto result = ProposedChangeSetParser::parse(
        R"({"schemaVersion":"proposal-submission.v2","summary":"s"})", makeMeta());
    assert(!result.success());
    assert(result.error()->code == "proposal.missing_field");
}

void testParseMissingSchemaVersion()
{
    const auto result = ProposedChangeSetParser::parse(
        R"({"summary":"s","changes":[]})", makeMeta());
    assert(!result.success());
    assert(result.error()->code == "proposal.missing_field");
}

void testParseWrongSchemaVersionIsRejected()
{
    const auto result = ProposedChangeSetParser::parse(
        R"({"schemaVersion":"proposal.v1","summary":"s","changes":[]})", makeMeta());
    assert(!result.success());
    assert(result.error()->code == "proposal.unsupported_schema");
}

void testParseUnknownOperation()
{
    const char* raw = R"({
        "schemaVersion": "proposal-submission.v2",
        "summary": "Bad op",
        "changes": [{
            "change_id": "chg-1",
            "operation": "move",
            "relative_path": "foo.cpp",
            "source_path": null,
            "proposed_content": null
        }]
    })";
    const auto result = ProposedChangeSetParser::parse(raw, makeMeta());
    assert(!result.success());
    assert(result.error()->code == "proposal.invalid_operation");
}

void testParseEmptyChangeId()
{
    const char* raw = R"({
        "schemaVersion": "proposal-submission.v2",
        "summary": "Empty id",
        "changes": [{
            "change_id": "",
            "operation": "create",
            "relative_path": "foo.cpp",
            "source_path": null,
            "proposed_content": "x"
        }]
    })";
    const auto result = ProposedChangeSetParser::parse(raw, makeMeta());
    assert(!result.success());
    assert(result.error()->code == "proposal.empty_change_id");
}

// Model tries to set run_id/change_set_id in the JSON. The exact-field-set
// enforcement now rejects this outright (proposal.unexpected_field) rather
// than silently ignoring the extra keys - a stronger property than the old
// contract's "ignored but tolerated": the model cannot even successfully
// submit a payload that attempts identity injection.
void testParseUnexpectedTopLevelFieldIsRejected()
{
    const char* raw = R"({
        "schemaVersion": "proposal-submission.v2",
        "run_id": "model-injected-run",
        "summary": "Metadata override attempt",
        "changes": []
    })";
    const auto result = ProposedChangeSetParser::parse(raw, makeMeta("run-001", "proj-001"));
    assert(!result.success());
    assert(result.error()->code == "proposal.unexpected_field");
    // The unknown field's own name/value must never be echoed.
    assert(result.error()->message.find("model-injected-run") == std::string::npos);
}

// A well-formed payload's trusted identity fields (runId/projectId) always
// come from the Backend-supplied TrustedChangeSetMetadata, never from
// anything in the model's JSON - there is no field in proposal-submission.v2
// through which the model could even attempt to supply them.
void testParseTrustedMetadataAlwaysWins()
{
    const char* raw =
        R"({"schemaVersion":"proposal-submission.v2","summary":"s","changes":[]})";
    const auto result = ProposedChangeSetParser::parse(raw, makeMeta("run-001", "proj-001"));
    assert(result.success());
    assert(result.value().runId == "run-001");
    assert(result.value().projectId == "proj-001");
    assert(!result.value().changeSetId.empty());
}

// expected_original_hash is not a recognized field at all in
// proposal-submission.v2: a model/provider that still sends one is rejected
// as an unexpected field before its type is ever inspected - never
// proposal.invalid_field_type, which this test previously exercised via that
// now-removed field.
void testParseRejectsOldHashFieldAsUnexpected()
{
    const char* raw = R"({
        "schemaVersion": "proposal-submission.v2",
        "summary": "Old hash field",
        "changes": [{
            "change_id": "chg-1",
            "operation": "modify",
            "relative_path": "src/main.cpp",
            "source_path": null,
            "expected_original_hash": "aabbccdd",
            "proposed_content": "updated"
        }]
    })";
    const auto result = ProposedChangeSetParser::parse(raw, makeMeta());
    assert(!result.success());
    assert(result.error()->code == "proposal.unexpected_field");
}

void testParseRejectsWrongOptionalFieldType()
{
    const char* raw = R"({
        "schemaVersion": "proposal-submission.v2",
        "summary": "Wrong optional type",
        "changes": [{
            "change_id": "chg-1",
            "operation": "modify",
            "relative_path": "src/main.cpp",
            "source_path": 123,
            "proposed_content": "updated"
        }]
    })";
    const auto result = ProposedChangeSetParser::parse(raw, makeMeta());
    assert(!result.success());
    assert(result.error()->code == "proposal.invalid_field_type");
}

// ---------------------------------------------------------------------------
// Validator tests
// ---------------------------------------------------------------------------

ProposedChangeSet makeSimpleCreate(
    const std::string& changeId,
    const std::string& relPath,
    const std::string& content = "hello")
{
    ProposedFileChange ch;
    ch.changeId = changeId;
    ch.operation = FileChangeOperation::Create;
    ch.relativePath = fs::path(relPath);
    ch.proposedContent = content;

    ProposedChangeSet cs;
    cs.schemaVersion = "proposal.v1";
    cs.changeSetId = "cs-test-001";
    cs.runId = "run-001";
    cs.projectId = "proj-001";
    cs.summary = "test";
    cs.changes = {ch};
    return cs;
}

ProjectWritePolicy makePermissivePolicy(const fs::path& root = fs::path("/fake/root"))
{
    ProjectWritePolicy p;
    p.projectRoot = root;
    p.allowDelete = true;
    p.allowRename = true;
    return p;
}

// ---------------------------------------------------------------------------
// Strict nullable-field and Backend-boundary tests.
// ---------------------------------------------------------------------------

// proposal-submission.v2 says inapplicable nullable fields must be JSON null.
// The Backend parser is the trust boundary and must not silently rewrite a
// provider that violated that contract.
void testParseCreateWithEmptySourcePathIsRejected()
{
    const char* raw = R"({
        "schemaVersion": "proposal-submission.v2",
        "summary": "Document QML project context",
        "changes": [{
            "change_id": "create-qml-project-context",
            "operation": "create",
            "relative_path": "QML_PROJECT_CONTEXT.md",
            "source_path": "",
            "proposed_content": "# QML Project Context"
        }]
    })";

    const auto result = ProposedChangeSetParser::parse(raw, makeMeta());
    assert(!result.success());
    assert(result.error()->code == "proposal.invalid_field_value");
}

// An empty proposed_content is a legitimate, deliberate value for Create
// (e.g. a .gitkeep-style empty marker file, which the runner's own system
// prompt instructs the model to use for creating folders) - it must NOT be
// rejected like the inapplicable placeholder field above.
void testParseCreateWithEmptyProposedContentIsPreserved()
{
    const char* raw = R"({
        "schemaVersion": "proposal-submission.v2",
        "summary": "Create empty marker file",
        "changes": [{
            "change_id": "create-gitkeep",
            "operation": "create",
            "relative_path": "Test/.gitkeep",
            "source_path": null,
            "proposed_content": ""
        }]
    })";

    const auto result = ProposedChangeSetParser::parse(raw, makeMeta());
    assert(result.success());
    const auto& ch = result.value().changes[0];
    assert(ch.proposedContent.has_value());
    assert(ch.proposedContent->empty());

    const auto findings = ChangeProposalValidator::validate(result.value(), makePermissivePolicy(), {});
    for (const auto& f : findings) {
        assert(f.code != "proposal.invariant_violated");
    }
}

void testParseDeleteWithEmptyProposedContentIsRejected()
{
    const char* raw = R"({
        "schemaVersion": "proposal-submission.v2",
        "summary": "Delete stale file",
        "changes": [{
            "change_id": "delete-stale",
            "operation": "delete",
            "relative_path": "src/old.cpp",
            "source_path": null,
            "proposed_content": ""
        }]
    })";

    const auto result = ProposedChangeSetParser::parse(raw, makeMeta());
    assert(!result.success());
    assert(result.error()->code == "proposal.invalid_field_value");
}

void testParseRenameWithEmptyProposedContentIsRejected()
{
    const char* raw = R"({
        "schemaVersion": "proposal-submission.v2",
        "summary": "Rename file",
        "changes": [{
            "change_id": "rename-file",
            "operation": "rename",
            "relative_path": "src/new.cpp",
            "source_path": "src/old.cpp",
            "proposed_content": ""
        }]
    })";

    const auto result = ProposedChangeSetParser::parse(raw, makeMeta());
    assert(!result.success());
    assert(result.error()->code == "proposal.invalid_field_value");
}

void testParserEnforcesAdvertisedStringBounds()
{
    const auto proposalWith = [](std::string summary, std::string changeId,
                                  std::string path, std::string content) {
        return std::string(R"({"schemaVersion":"proposal-submission.v2","summary":")")
            + summary + R"(","changes":[{"change_id":")" + changeId
            + R"(","operation":"create","relative_path":")" + path
            + R"(","source_path":null,"proposed_content":")" + content
            + R"("}]})";
    };

    auto result = ProposedChangeSetParser::parse(
        proposalWith(std::string(kProposalSubmissionMaxSummaryBytes + 1, 's'),
            "id", "file.txt", "x"), makeMeta());
    assert(!result.success() && result.error()->code == "proposal.field_too_large");

    result = ProposedChangeSetParser::parse(
        proposalWith("summary",
            std::string(kProposalSubmissionMaxChangeIdBytes + 1, 'a'),
            "file.txt", "x"), makeMeta());
    assert(!result.success() && result.error()->code == "proposal.field_too_large");

    result = ProposedChangeSetParser::parse(
        proposalWith("summary", "id",
            std::string(kProposalSubmissionMaxPathBytes + 1, 'p'), "x"), makeMeta());
    assert(!result.success() && result.error()->code == "proposal.field_too_large");

    result = ProposedChangeSetParser::parse(
        proposalWith("summary", "id", "file.txt",
            std::string(kProposalSubmissionMaxContentBytes + 1, 'x')), makeMeta());
    assert(!result.success() && result.error()->code == "proposal.field_too_large");
}

void testValidatePathTraversal()
{
    auto cs = makeSimpleCreate("chg-1", "src/../../../etc/passwd");
    const auto findings = ChangeProposalValidator::validate(cs, makePermissivePolicy(), {});
    assert(!findings.empty());
    bool found = false;
    for (const auto& f : findings) {
        if (f.code == "proposal.path_traversal") { found = true; break; }
    }
    assert(found);
}

void testValidateAbsolutePath()
{
    auto cs = makeSimpleCreate("chg-1", "/etc/passwd");
    const auto findings = ChangeProposalValidator::validate(cs, makePermissivePolicy(), {});
    bool found = false;
    for (const auto& f : findings) {
        if (f.code == "proposal.absolute_path") { found = true; break; }
    }
    assert(found);
}

void testValidateGitPath()
{
    auto cs = makeSimpleCreate("chg-1", ".git/config");
    const auto findings = ChangeProposalValidator::validate(cs, makePermissivePolicy(), {});
    bool found = false;
    for (const auto& f : findings) {
        if (f.code == "proposal.git_path") { found = true; break; }
    }
    assert(found);
}

void testValidateSecretFilename()
{
    auto cs = makeSimpleCreate("chg-1", ".env");
    const auto findings = ChangeProposalValidator::validate(cs, makePermissivePolicy(), {});
    bool found = false;
    for (const auto& f : findings) {
        if (f.code == "proposal.secret_file") { found = true; break; }
    }
    assert(found);
}

void testValidateBuildDir()
{
    auto cs = makeSimpleCreate("chg-1", "build/output.o");
    const auto findings = ChangeProposalValidator::validate(cs, makePermissivePolicy(), {});
    bool found = false;
    for (const auto& f : findings) {
        if (f.code == "proposal.build_path") { found = true; break; }
    }
    assert(found);
}

void testValidateAllowedRootsViolation()
{
    auto cs = makeSimpleCreate("chg-1", "other/file.cpp");
    ProjectWritePolicy policy;
    policy.projectRoot = "/fake/root";
    policy.allowedRoots = {fs::path("src")};
    const auto findings = ChangeProposalValidator::validate(cs, policy, {});
    bool found = false;
    for (const auto& f : findings) {
        if (f.code == "proposal.outside_allowed_roots") { found = true; break; }
    }
    assert(found);
}

void testValidateNestedAllowedRoot()
{
    auto cs = makeSimpleCreate("chg-1", "src/application/service.cpp");
    ProjectWritePolicy policy;
    policy.allowedRoots = {fs::path("src/application")};
    const auto findings = ChangeProposalValidator::validate(cs, policy, {});
    for (const auto& f : findings) {
        assert(f.code != "proposal.outside_allowed_roots");
    }
}

void testValidateDeniedPath()
{
    auto cs = makeSimpleCreate("chg-1", "src/private/secret.cpp");
    ProjectWritePolicy policy;
    policy.projectRoot = "/fake/root";
    policy.deniedPaths = {fs::path("src/private")};
    const auto findings = ChangeProposalValidator::validate(cs, policy, {});
    bool found = false;
    for (const auto& f : findings) {
        if (f.code == "proposal.denied_path") { found = true; break; }
    }
    assert(found);
}

void testValidateRenameSourcePolicy()
{
    ProposedFileChange ch;
    ch.changeId = "chg-rename";
    ch.operation = FileChangeOperation::Rename;
    ch.relativePath = "src/public/new.cpp";
    ch.sourcePath = fs::path("src/private/old.cpp");
    ch.expectedOriginalHash = std::string(64, 'a');

    ProposedChangeSet cs;
    cs.summary = "Rename denied source";
    cs.changes = {ch};

    ProjectWritePolicy policy;
    policy.allowRename = true;
    policy.allowedRoots = {fs::path("src")};
    policy.deniedPaths = {fs::path("src/private")};

    const auto findings = ChangeProposalValidator::validate(cs, policy, {});
    bool found = false;
    for (const auto& f : findings) {
        if (f.code == "proposal.denied_path") { found = true; break; }
    }
    assert(found);
}

#ifdef _WIN32
void testValidateWindowsCaseInsensitiveProtectedPaths()
{
    auto gitChange = makeSimpleCreate("chg-git", ".GIT/config");
    auto envChange = makeSimpleCreate("chg-env", ".ENV");

    const auto gitFindings =
        ChangeProposalValidator::validate(gitChange, ProjectWritePolicy {}, {});
    const auto envFindings =
        ChangeProposalValidator::validate(envChange, ProjectWritePolicy {}, {});

    assert(std::any_of(gitFindings.begin(), gitFindings.end(),
        [](const ValidationFinding& f) { return f.code == "proposal.git_path"; }));
    assert(std::any_of(envFindings.begin(), envFindings.end(),
        [](const ValidationFinding& f) { return f.code == "proposal.secret_file"; }));
}
#endif

void testValidateExtensionNotAllowed()
{
    auto cs = makeSimpleCreate("chg-1", "src/main.exe");
    ProjectWritePolicy policy;
    policy.projectRoot = "/fake/root";
    policy.allowedExtensions = {".cpp", ".hpp"};
    const auto findings = ChangeProposalValidator::validate(cs, policy, {});
    bool found = false;
    for (const auto& f : findings) {
        if (f.code == "proposal.extension_not_allowed") { found = true; break; }
    }
    assert(found);
}

void testValidateMaxFilesExceeded()
{
    ProposedChangeSet cs;
    cs.schemaVersion = "proposal.v1";
    cs.changeSetId = "cs-001";
    cs.runId = "run-001";
    cs.projectId = "proj-001";
    cs.summary = "Many files";
    for (int i = 0; i < 3; ++i) {
        ProposedFileChange ch;
        ch.changeId = "chg-" + std::to_string(i);
        ch.operation = FileChangeOperation::Create;
        ch.relativePath = "src/file" + std::to_string(i) + ".cpp";
        ch.proposedContent = "x";
        cs.changes.push_back(ch);
    }
    ProjectWritePolicy policy;
    policy.projectRoot = "/fake/root";
    policy.maxFilesPerChangeSet = 2; // only 2 allowed
    const auto findings = ChangeProposalValidator::validate(cs, policy, {});
    bool found = false;
    for (const auto& f : findings) {
        if (f.code == "proposal.too_many_files") { found = true; break; }
    }
    assert(found);
}

void testValidateMaxBytesExceeded()
{
    auto cs = makeSimpleCreate("chg-1", "src/big.cpp",
        std::string(1024, 'x')); // 1024 bytes
    ProjectWritePolicy policy;
    policy.projectRoot = "/fake/root";
    policy.maxTotalBytes = 100; // only 100 bytes allowed
    const auto findings = ChangeProposalValidator::validate(cs, policy, {});
    bool found = false;
    for (const auto& f : findings) {
        if (f.code == "proposal.too_large") { found = true; break; }
    }
    assert(found);
}

void testValidateDeleteWithoutPermission()
{
    ProposedFileChange ch;
    ch.changeId = "chg-1";
    ch.operation = FileChangeOperation::Delete;
    ch.relativePath = "src/foo.cpp";
    ch.expectedOriginalHash = "aabbccdd";

    ProposedChangeSet cs;
    cs.schemaVersion = "proposal.v1";
    cs.changeSetId = "cs-001";
    cs.summary = "Delete";
    cs.changes = {ch};

    ProjectWritePolicy policy;
    policy.projectRoot = "/fake/root";
    policy.allowDelete = false;
    const auto findings = ChangeProposalValidator::validate(cs, policy, {});
    bool found = false;
    for (const auto& f : findings) {
        if (f.code == "proposal.delete_not_permitted") { found = true; break; }
    }
    assert(found);
}

void testValidateRenameWithoutPermission()
{
    ProposedFileChange ch;
    ch.changeId = "chg-1";
    ch.operation = FileChangeOperation::Rename;
    ch.relativePath = "src/new.cpp";
    ch.sourcePath = fs::path("src/old.cpp");
    ch.expectedOriginalHash = "aabbccdd";

    ProposedChangeSet cs;
    cs.schemaVersion = "proposal.v1";
    cs.changeSetId = "cs-001";
    cs.summary = "Rename";
    cs.changes = {ch};

    ProjectWritePolicy policy;
    policy.projectRoot = "/fake/root";
    policy.allowRename = false;
    const auto findings = ChangeProposalValidator::validate(cs, policy, {});
    bool found = false;
    for (const auto& f : findings) {
        if (f.code == "proposal.rename_not_permitted") { found = true; break; }
    }
    assert(found);
}

void testValidateDuplicateChangeId()
{
    ProposedChangeSet cs;
    cs.schemaVersion = "proposal.v1";
    cs.changeSetId = "cs-001";
    cs.summary = "Dup";
    for (int i = 0; i < 2; ++i) {
        ProposedFileChange ch;
        ch.changeId = "same-id";
        ch.operation = FileChangeOperation::Create;
        ch.relativePath = "src/file" + std::to_string(i) + ".cpp";
        ch.proposedContent = "x";
        cs.changes.push_back(ch);
    }
    const auto findings = ChangeProposalValidator::validate(cs, makePermissivePolicy(), {});
    bool found = false;
    for (const auto& f : findings) {
        if (f.code == "proposal.duplicate_change_id") { found = true; break; }
    }
    assert(found);
}

void testValidateConflictingPaths()
{
    ProposedChangeSet cs;
    cs.schemaVersion = "proposal.v1";
    cs.changeSetId = "cs-001";
    cs.summary = "Conflict";
    for (int i = 0; i < 2; ++i) {
        ProposedFileChange ch;
        ch.changeId = "chg-" + std::to_string(i);
        ch.operation = FileChangeOperation::Create;
        ch.relativePath = "src/same.cpp"; // same path
        ch.proposedContent = "x";
        cs.changes.push_back(ch);
    }
    const auto findings = ChangeProposalValidator::validate(cs, makePermissivePolicy(), {});
    bool found = false;
    for (const auto& f : findings) {
        if (f.code == "proposal.conflicting_path") { found = true; break; }
    }
    assert(found);
}

void testValidateBinaryContent()
{
    std::string content = "hello";
    content += '\0'; // NUL byte makes it binary
    content += "world";
    auto cs = makeSimpleCreate("chg-1", "src/bin.cpp", content);
    const auto findings = ChangeProposalValidator::validate(cs, makePermissivePolicy(), {});
    bool found = false;
    for (const auto& f : findings) {
        if (f.code == "proposal.binary_not_supported") { found = true; break; }
    }
    assert(found);
}

void testValidateCreateWithoutContent()
{
    ProposedFileChange ch;
    ch.changeId = "chg-1";
    ch.operation = FileChangeOperation::Create;
    ch.relativePath = "src/foo.cpp";
    // no proposedContent

    ProposedChangeSet cs;
    cs.schemaVersion = "proposal.v1";
    cs.changeSetId = "cs-001";
    cs.summary = "No content";
    cs.changes = {ch};

    const auto findings = ChangeProposalValidator::validate(cs, makePermissivePolicy(), {});
    bool found = false;
    for (const auto& f : findings) {
        // Distinct from proposal.invariant_violated: this is a shape mistake
        // ReadOnlyAnalysisWorkflowRunner::isModelFormatError() treats as
        // retriable, unlike the other per-operation invariants below.
        if (f.code == "proposal.missing_proposed_content") { found = true; break; }
    }
    assert(found);
}

void testValidateModifyWithoutHash()
{
    ProposedFileChange ch;
    ch.changeId = "chg-1";
    ch.operation = FileChangeOperation::Modify;
    ch.relativePath = "src/foo.cpp";
    ch.proposedContent = "new";
    // no expectedOriginalHash

    ProposedChangeSet cs;
    cs.schemaVersion = "proposal.v1";
    cs.changeSetId = "cs-001";
    cs.summary = "No hash";
    cs.changes = {ch};

    const auto findings = ChangeProposalValidator::validate(cs, makePermissivePolicy(), {});
    bool found = false;
    for (const auto& f : findings) {
        if (f.code == "proposal.invariant_violated") { found = true; break; }
    }
    assert(found);
}

// Reproduces the exact real-world rejection this fix targets: a Modify
// change whose proposed_content was omitted (returned as JSON null by the
// model). Must surface as proposal.missing_proposed_content, not the
// generic proposal.invariant_violated, so the runner's repair retry can
// recognize and recover from it.
void testValidateModifyWithoutContent()
{
    ProposedFileChange ch;
    ch.changeId = "chg-1";
    ch.operation = FileChangeOperation::Modify;
    ch.relativePath = "src/foo.cpp";
    ch.expectedOriginalHash = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    // no proposedContent

    ProposedChangeSet cs;
    cs.schemaVersion = "proposal.v1";
    cs.changeSetId = "cs-001";
    cs.summary = "No content";
    cs.changes = {ch};

    const auto findings = ChangeProposalValidator::validate(cs, makePermissivePolicy(), {});
    bool found = false;
    for (const auto& f : findings) {
        if (f.code == "proposal.missing_proposed_content") { found = true; break; }
    }
    assert(found);
}

void testValidateHashMismatch()
{
    ProposedFileChange ch;
    ch.changeId = "chg-1";
    ch.operation = FileChangeOperation::Modify;
    ch.relativePath = "src/foo.cpp";
    ch.proposedContent = "new content";
    ch.expectedOriginalHash = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    ch.capturedOriginalContent = "actual file content"; // hash won't match

    ProposedChangeSet cs;
    cs.schemaVersion = "proposal.v1";
    cs.changeSetId = "cs-001";
    cs.summary = "Hash mismatch";
    cs.changes = {ch};

    const auto findings = ChangeProposalValidator::validate(cs, makePermissivePolicy(), {});
    bool found = false;
    for (const auto& f : findings) {
        if (f.code == "proposal.hash_mismatch") { found = true; break; }
    }
    assert(found);
}

void testValidateHashMatch()
{
    const std::string originalContent = "hello world";
    const std::string hash = ai::application::sha256Hex(originalContent);

    ProposedFileChange ch;
    ch.changeId = "chg-1";
    ch.operation = FileChangeOperation::Modify;
    ch.relativePath = "src/foo.cpp";
    ch.proposedContent = "new content";
    ch.expectedOriginalHash = hash;
    ch.capturedOriginalContent = originalContent;

    ProposedChangeSet cs;
    cs.schemaVersion = "proposal.v1";
    cs.changeSetId = "cs-001";
    cs.summary = "Hash match";
    cs.changes = {ch};

    const auto findings = ChangeProposalValidator::validate(cs, makePermissivePolicy(), {});
    for (const auto& f : findings) {
        assert(f.code != "proposal.hash_mismatch");
    }
}

// Part of the P0 fix for Run f5058a12-746a-4ab6-8deb-ca5bfc5500ca: when a
// caller sets requireHashEvidence (AgentToolTurnLoopRunner does, for every
// real agent tool session), a Modify/Delete/Rename hash is checked against
// observedFileHashes - a record of real workspace_hash_file tool results -
// not the change's own expectedOriginalHash string. A path with no matching
// evidence entry fails closed with proposal.missing_hash_evidence, even
// though the model's own claimed hash here happens to be correct: an
// unverified claim is never trusted on its own.
void testValidateHashEvidenceMissingIsRejected()
{
    const std::string originalContent = "actual file content";

    ProposedFileChange ch;
    ch.changeId = "chg-1";
    ch.operation = FileChangeOperation::Modify;
    ch.relativePath = "src/foo.cpp";
    ch.proposedContent = "new content";
    ch.expectedOriginalHash = ai::application::sha256Hex(originalContent);
    ch.capturedOriginalContent = originalContent;

    ProposedChangeSet cs;
    cs.schemaVersion = "proposal.v1";
    cs.changeSetId = "cs-001";
    cs.summary = "Evidence missing";
    cs.changes = {ch};

    const auto findings = ChangeProposalValidator::validate(
        cs, makePermissivePolicy(), {}, /*requireHashEvidence=*/true, {});
    bool found = false;
    for (const auto& f : findings) {
        if (f.code == "proposal.missing_hash_evidence") { found = true; break; }
    }
    assert(found);
}

// The practical payoff: a real observed hash for the change's path overrides
// a wrong/typo'd expectedOriginalHash string entirely - a transcription slip
// no longer sinks an otherwise-legitimate, actually-inspected change.
void testValidateHashEvidenceOverridesWrongModelString()
{
    const std::string originalContent = "hello world";
    const std::string realHash = ai::application::sha256Hex(originalContent);

    ProposedFileChange ch;
    ch.changeId = "chg-1";
    ch.operation = FileChangeOperation::Modify;
    ch.relativePath = "src/foo.cpp";
    ch.proposedContent = "new content";
    ch.expectedOriginalHash = std::string(64, '0'); // wrong; must be ignored
    ch.capturedOriginalContent = originalContent;

    ProposedChangeSet cs;
    cs.schemaVersion = "proposal.v1";
    cs.changeSetId = "cs-001";
    cs.summary = "Evidence overrides typo";
    cs.changes = {ch};

    std::unordered_map<std::string, std::string> evidence;
    evidence["src/foo.cpp"] = realHash;

    const auto findings = ChangeProposalValidator::validate(
        cs, makePermissivePolicy(), {}, /*requireHashEvidence=*/true, evidence);
    for (const auto& f : findings) {
        assert(f.code != "proposal.hash_mismatch");
        assert(f.code != "proposal.missing_hash_evidence");
    }
}

// Evidence-backed hashing must not weaken the pre-existing stale-world/
// concurrency protection: if the file changed on disk after the agent
// observed it, the change is still rejected, even though the model
// faithfully copied the hash it was actually given at the time.
void testValidateHashEvidenceStaleFileStillRejected()
{
    const std::string observedContent = "hello world";     // what the agent hashed
    const std::string currentContent = "hello world v2";   // changed since then
    const std::string observedHash = ai::application::sha256Hex(observedContent);

    ProposedFileChange ch;
    ch.changeId = "chg-1";
    ch.operation = FileChangeOperation::Modify;
    ch.relativePath = "src/foo.cpp";
    ch.proposedContent = "new content";
    ch.expectedOriginalHash = observedHash; // correctly copied
    ch.capturedOriginalContent = currentContent; // but the file has since changed

    ProposedChangeSet cs;
    cs.schemaVersion = "proposal.v1";
    cs.changeSetId = "cs-001";
    cs.summary = "Stale evidence";
    cs.changes = {ch};

    std::unordered_map<std::string, std::string> evidence;
    evidence["src/foo.cpp"] = observedHash;

    const auto findings = ChangeProposalValidator::validate(
        cs, makePermissivePolicy(), {}, /*requireHashEvidence=*/true, evidence);
    bool found = false;
    for (const auto& f : findings) {
        if (f.code == "proposal.hash_mismatch") { found = true; break; }
    }
    assert(found);
}

void testValidateCleanProposalNoFindings()
{
    auto cs = makeSimpleCreate("chg-1", "src/hello.cpp", "int main() {}");
    const auto findings = ChangeProposalValidator::validate(
        cs, makePermissivePolicy(), {});
    assert(findings.empty());
}

// ---------------------------------------------------------------------------
// Secret scanner tests
// ---------------------------------------------------------------------------

ProposedChangeSet makeCs(const std::string& summary, const std::string& content = {})
{
    ProposedChangeSet cs;
    cs.schemaVersion = "proposal.v1";
    cs.changeSetId = "cs-secret-001";
    cs.runId = "run-001";
    cs.projectId = "proj-001";
    cs.summary = summary;
    if (!content.empty()) {
        ProposedFileChange ch;
        ch.changeId = "chg-s1";
        ch.operation = FileChangeOperation::Create;
        ch.relativePath = "src/config.cpp";
        ch.proposedContent = content;
        cs.changes.push_back(ch);
    }
    return cs;
}

void testSecretScanAwsKeyInContent()
{
    // AWS Access Key format: AKIA + 16 alphanumeric chars
    const auto cs = makeCs("Update config", "AKIAIOSFODNN7EXAMPLE12345678");
    const auto findings = ChangeProposalValidator::scanForSecrets(cs);
    assert(!findings.empty());
    assert(findings[0].code == "proposal.secret_detected");
    // Must not echo the key value in the message
    assert(findings[0].message.find("AKIA") == std::string::npos);
}

void testSecretScanOpenAiKeyInSummary()
{
    // OpenAI key format: sk- + 32+ alphanumeric chars
    const auto cs = makeCs("Key=sk-abcdefghijklmnopqrstuvwxyz12345678");
    const auto findings = ChangeProposalValidator::scanForSecrets(cs);
    assert(!findings.empty());
    assert(findings[0].code == "proposal.secret_detected");
    // Must not echo the key
    assert(findings[0].message.find("sk-") == std::string::npos);
}

void testSecretScanPemKeyInContent()
{
    const auto cs = makeCs("Update certs",
        "-----BEGIN RSA PRIVATE KEY-----\nMIIEowIBAAK...\n-----END RSA PRIVATE KEY-----\n");
    const auto findings = ChangeProposalValidator::scanForSecrets(cs);
    assert(!findings.empty());
    assert(findings[0].code == "proposal.secret_detected");
}

void testSecretScanCleanContentPasses()
{
    const auto cs = makeCs("Add README", "# Hello World\n\nThis is a documentation file.");
    const auto findings = ChangeProposalValidator::scanForSecrets(cs);
    assert(findings.empty());
}

void testSecretScanNeverEchosValue()
{
    const std::string secretKey = "AKIAIOSFODNN7EXAMPLE12345678";
    const auto cs = makeCs("Config update", "key=" + secretKey);
    const auto findings = ChangeProposalValidator::scanForSecrets(cs);
    assert(!findings.empty());
    for (const auto& f : findings) {
        // The exact secret value must never appear in any finding field
        assert(f.message.find(secretKey) == std::string::npos);
        assert(f.code.find(secretKey) == std::string::npos);
    }
}

// ---------------------------------------------------------------------------
// Store tests
// ---------------------------------------------------------------------------

ProposedChangeSet makeFullChangeSet(
    const std::string& changeSetId,
    const std::string& runId,
    const fs::path& /*unused*/ = {})
{
    ProposedFileChange ch;
    ch.changeId = "chg-store-1";
    ch.operation = FileChangeOperation::Create;
    ch.relativePath = "src/foo.cpp";
    ch.proposedContent = "int main() { return 0; }";
    ch.capturedOriginalContent = std::nullopt;

    ProposedChangeSet cs;
    cs.schemaVersion = "proposal.v1";
    cs.changeSetId = changeSetId;
    cs.runId = runId;
    cs.projectId = "proj-001";
    cs.providerId = "openai";
    cs.modelId = "gpt-4";
    cs.agentRole = "code-agent";
    cs.summary = "Add main.cpp";
    cs.baseRevision = "main";
    cs.state = ChangeSetState::Validated;
    cs.createdAt = std::chrono::system_clock::now();
    cs.changes = {ch};
    return cs;
}

void testStorePublishAndReadContent()
{
    TempDir tmp;
    auto storeResult = FileChangeProposalStore::open(tmp.path / "proposals");
    assert(storeResult.success());
    auto& store = *storeResult.value();

    ProjectWritePolicy policy;
    policy.projectRoot = tmp.path / "project";

    auto cs = makeFullChangeSet("cs-pub-001", "run-001");
    cs.changes[0].capturedOriginalContent = "original content";

    auto loc = store.publishChangeSet(cs, policy);
    assert(loc.success());
    assert(!loc.value().artifactRelativeRoot.empty());
    assert(!loc.value().policySnapshotHash.empty());

    // Read proposed content
    const auto proposed = store.readProposedContent(
        loc.value().artifactRelativeRoot, "chg-store-1");
    assert(proposed.success());
    assert(proposed.value() == "int main() { return 0; }");

    // Read original content
    const auto original = store.readOriginalContent(
        loc.value().artifactRelativeRoot, "chg-store-1");
    assert(original.success());
    assert(original.value() == "original content");
}

void testStoreDuplicateChangeSetIdRejected()
{
    TempDir tmp;
    auto storeResult = FileChangeProposalStore::open(tmp.path / "proposals");
    assert(storeResult.success());
    auto& store = *storeResult.value();

    ProjectWritePolicy policy;
    policy.projectRoot = "/fake/root";

    auto cs = makeFullChangeSet("cs-dup-001", "run-001");
    cs.changes[0].capturedOriginalContent = std::nullopt;

    assert(store.publishChangeSet(cs, policy).success());
    // Second publish with same changeSetId must fail
    const auto second = store.publishChangeSet(cs, policy);
    assert(!second.success());
    assert(second.error()->code == "proposal_store.duplicate_id");
}

void testStoreUnsafeIdRejected()
{
    TempDir tmp;
    auto storeResult = FileChangeProposalStore::open(tmp.path / "proposals");
    assert(storeResult.success());
    auto& store = *storeResult.value();

    ProjectWritePolicy policy;
    policy.projectRoot = "/fake/root";

    auto cs = makeFullChangeSet("../escape/id", "run-001");
    const auto result = store.publishChangeSet(cs, policy);
    assert(!result.success());
    assert(result.error()->code == "proposal_store.unsafe_id");
}

// ---------------------------------------------------------------------------
// Store: paths longer than the legacy 260-character limit
//
// A published proposal keeps full SHA-256 file names, so its files sit 164 characters below the store root
// (\runs\<uuid>\proposals\<uuid>\originals|proposed\<sha256>). The whole publish -> read -> reopen cycle must work when
// a file path, then a directory path, then the root itself is longer than MAX_PATH, and the containment checks must
// still refuse a path that leaves the root. (This test's own helpers use the extended-length form on Windows.)
// ---------------------------------------------------------------------------

fs::path extendedForTest(const fs::path& path)
{
#ifdef _WIN32
    const std::wstring value = fs::absolute(path).native();
    if (value.rfind(LR"(\\?\)", 0) == 0) return fs::path(value);
    return fs::path(std::wstring(LR"(\\?\)") + value);
#else
    return path;
#endif
}

struct LongStoreRoot {
    fs::path base;   // short, under TEMP: removed with the extended-length form
    fs::path root;   // the store root: at least `minimumLength` characters

    explicit LongStoreRoot(std::size_t minimumLength)
    {
        base = fs::temp_directory_path()
            / ("aio-longpath-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        root = base;
        int segment = 0;
        while (root.native().size() + 12 < minimumLength) {
            root /= "seg" + std::to_string(segment++) + std::string(36, 'x');
        }
        root /= "proposals";
    }

    ~LongStoreRoot()
    {
        std::error_code ec;
        fs::remove_all(extendedForTest(base), ec);
        assert(!fs::exists(extendedForTest(base)) && "the long test tree must be removed completely");
    }
};

void writeOutsideTheStore(const fs::path& directory, const std::string& name, const std::string& content)
{
    std::error_code ec;
    fs::create_directories(extendedForTest(directory), ec);
    assert(!ec);
    std::ofstream out(extendedForTest(directory / name), std::ios::binary | std::ios::trunc);
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
    assert(static_cast<bool>(out));
}

// A failed step says WHICH Backend error it was (the message of a bare assert would not).
template <typename ResultType>
void requireStoreOk(const ResultType& result, const char* step, std::size_t rootLength)
{
    if (!result.success()) {
        std::fprintf(stderr, "[long-path store, root >= %zu] %s failed: %s / %s\n", rootLength, step,
            result.error()->code.c_str(), result.error()->message.c_str());
    }
    assert(result.success() && "see the line above for the failing step and its error");
}

void runStoreLongPathCycle(std::size_t minimumRootLength)
{
    LongStoreRoot tree(minimumRootLength);
    ProjectWritePolicy policy;
    policy.projectRoot = tree.base / "project";

    ProposedChangeSet cs = makeFullChangeSet("00000000-aaaa-bbbb-cccc-000000000001", "11111111-aaaa-bbbb-cccc-000000000001");
    cs.changes[0].capturedOriginalContent = "original content";

    std::string relativeRoot;
    {
        auto opened = FileChangeProposalStore::open(tree.root);
        requireStoreOk(opened, "open", minimumRootLength);
        auto& store = *opened.value();
        const auto published = store.publishChangeSet(cs, policy);
        requireStoreOk(published, "publish", minimumRootLength);
        relativeRoot = published.value().artifactRelativeRoot;

        // The test really is about a long path: the proposed file is longer than the legacy limit.
        const fs::path proposedFile = tree.root / fs::path(relativeRoot) / "proposed" / sha256Hex(cs.changes[0].changeId);
        assert(proposedFile.native().size() > 260 && "this case must exercise a file path longer than MAX_PATH");

        const auto proposed = store.readProposedContent(relativeRoot, "chg-store-1");
        requireStoreOk(proposed, "read proposed", minimumRootLength);
        assert(proposed.value() == "int main() { return 0; }");
        const auto original = store.readOriginalContent(relativeRoot, "chg-store-1");
        requireStoreOk(original, "read original", minimumRootLength);
        assert(original.value() == "original content");

        // A second publish with the same id is refused by the existence check on a long path too.
        const auto duplicate = store.publishChangeSet(cs, policy);
        assert(!duplicate.success() && duplicate.error()->code == "proposal_store.duplicate_id");
    }
    {
        // A fresh store object over the same root (an application restart) reads what was published and keeps publishing.
        auto reopened = FileChangeProposalStore::open(tree.root);
        requireStoreOk(reopened, "reopen", minimumRootLength);
        auto& store = *reopened.value();
        const auto proposed = store.readProposedContent(relativeRoot, "chg-store-1");
        requireStoreOk(proposed, "read proposed after reopen", minimumRootLength);
        assert(proposed.value() == "int main() { return 0; }");
        const auto original = store.readOriginalContent(relativeRoot, "chg-store-1");
        requireStoreOk(original, "read original after reopen", minimumRootLength);
        assert(original.value() == "original content");

        ProposedChangeSet second = makeFullChangeSet("00000000-aaaa-bbbb-cccc-000000000002", "11111111-aaaa-bbbb-cccc-000000000001");
        second.changes[0].proposedContent = "second";
        const auto secondPublished = store.publishChangeSet(second, policy);
        requireStoreOk(secondPublished, "publish after reopen", minimumRootLength);
        const auto secondRead = store.readProposedContent(secondPublished.value().artifactRelativeRoot, "chg-store-1");
        requireStoreOk(secondRead, "read the second proposal", minimumRootLength);
        assert(secondRead.value() == "second");

        // Content that does not exist is "not found", never a made-up success.
        const auto missing = store.readProposedContent(relativeRoot, "no-such-change");
        assert(!missing.success() && missing.error()->code == "proposal.not_found");

        // Containment still holds on a long path: a real file OUTSIDE the root, reached through "..", is refused.
        const fs::path outside = tree.root.parent_path() / "outside";
        writeOutsideTheStore(outside / "proposed", sha256Hex("chg-store-1"), "stolen");
        const auto escaped = store.readProposedContent("../outside", "chg-store-1");
        assert(!escaped.success() && "a path that leaves the store root must not be read");
        assert(escaped.error()->code == "proposal_store.path_escape");
        const auto escapedOriginal = store.readOriginalContent("../outside", "chg-store-1");
        assert(!escapedOriginal.success());
    }
}

void testStoreSupportsPathsLongerThanMaxPath()
{
    runStoreLongPathCycle(100);   // files longer than 260, directories still shorter
    runStoreLongPathCycle(140);   // the staging directory is longer than 260 too
    runStoreLongPathCycle(300);   // the root itself is longer than 260
}

// ---------------------------------------------------------------------------
// Service integration tests
// ---------------------------------------------------------------------------

CreateChangeProposalRequest makeServiceRequest(
    const fs::path& projectRoot,
    const std::string& rawJson,
    bool allowDelete = false,
    bool allowRename = false)
{
    fs::create_directories(projectRoot);
    CreateChangeProposalRequest req;
    req.runId = "run-svc-001";
    req.projectId = "proj-svc-001";
    req.providerId = "openai";
    req.modelId = "gpt-4";
    req.agentRole = "code-agent";
    req.rawModelOutput = rawJson;
    req.policy.projectRoot = projectRoot;
    req.policy.allowDelete = allowDelete;
    req.policy.allowRename = allowRename;
    req.baseRevision = "main";
    return req;
}

void testServiceCreateReadyForReview()
{
    TempDir tmp;
    FakeChangeProposalRepository repo;
    auto storeResult = FileChangeProposalStore::open(tmp.path / "proposals");
    assert(storeResult.success());
    ChangeProposalService svc(repo, *storeResult.value());

    // Create proposes a new file that does not exist yet in the project dir
    const std::string raw = R"({
        "schemaVersion": "proposal-submission.v2",
        "summary": "Add hello",
        "changes": [{
            "change_id": "chg-svc-1",
            "operation": "create",
            "relative_path": "src/hello.cpp",
            "source_path": null,
            "proposed_content": "int main() {}"
        }]
    })";

    auto req = makeServiceRequest(tmp.path / "project", raw);
    const auto result = svc.createFromModelOutput(req);
    if (!result.success()) {
        std::fprintf(
            stderr,
            "%s: %s\n",
            result.error()->code.c_str(),
            result.error()->message.c_str());
        throw std::runtime_error(
            result.error()->code + ": " + result.error()->message);
    }
    assert(result.success());
    assert(result.value().state == ChangeSetState::ReadyForReview);
    assert(result.value().validationFindings.empty());
    assert(!result.value().changeSetId.empty());
    assert(!result.value().artifactRelativeRoot.empty());
    assert(repo.size() == 1);
}

void testServiceCreateTargetExistsIsRejected()
{
    TempDir tmp;
    FakeChangeProposalRepository repo;
    auto storeResult = FileChangeProposalStore::open(tmp.path / "proposals");
    assert(storeResult.success());
    ChangeProposalService svc(repo, *storeResult.value());

    // Pre-create the file so the Create operation sees it as existing
    tmp.writeFile("project/src/hello.cpp", "existing content");

    const std::string raw = R"({
        "schemaVersion": "proposal-submission.v2",
        "summary": "Create existing file",
        "changes": [{
            "change_id": "chg-svc-2",
            "operation": "create",
            "relative_path": "src/hello.cpp",
            "source_path": null,
            "proposed_content": "new content"
        }]
    })";

    auto req = makeServiceRequest(tmp.path / "project", raw);
    const auto result = svc.createFromModelOutput(req);
    assert(result.success()); // service succeeds but proposal is Rejected
    assert(result.value().state == ChangeSetState::Rejected);
    bool found = false;
    for (const auto& f : result.value().validationFindings) {
        if (f.code == "proposal.create_target_exists") { found = true; break; }
    }
    assert(found);
}

// An empty changes[] is not a validation failure — it means the agent is
// reporting findings/analysis only, with nothing to change. It must land in
// its own terminal state, distinct from both ReadyForReview (nothing to
// review) and Rejected (nothing was actually wrong).
void testServiceCreateNoChangesNeeded()
{
    TempDir tmp;
    FakeChangeProposalRepository repo;
    auto storeResult = FileChangeProposalStore::open(tmp.path / "proposals");
    assert(storeResult.success());
    ChangeProposalService svc(repo, *storeResult.value());

    const std::string raw = R"({
        "schemaVersion": "proposal-submission.v2",
        "summary": "Reviewed the module; no changes are needed.",
        "changes": []
    })";

    auto req = makeServiceRequest(tmp.path / "project", raw);
    const auto result = svc.createFromModelOutput(req);
    assert(result.success());
    assert(result.value().state == ChangeSetState::NoChangesNeeded);
    assert(result.value().validationFindings.empty());
    assert(result.value().changes.empty());
    assert(!result.value().changeSetId.empty());
    assert(!result.value().artifactRelativeRoot.empty());
    assert(repo.size() == 1);
}

// Reproduces the multi-agent scenario ExecutionProfileWorkflowRunner will
// drive once workspace-access agents run in parallel: two DIFFERENT
// proposals (different changeSetId, same runId) racing for the same
// relative path. This is distinct from the intra-changeset
// proposal.conflicting_path check (ChangeProposalValidator.cpp), which can
// only ever see two changes inside ONE proposal.
void testCrossAgentConflictRejectsSecondProposalSamePath()
{
    TempDir tmp;
    FakeChangeProposalRepository repo;
    auto storeResult = FileChangeProposalStore::open(tmp.path / "proposals");
    assert(storeResult.success());
    RunPathClaimRegistry registry;
    ChangeProposalService svc(repo, *storeResult.value(), &registry);

    const std::string rawA = R"({
        "schemaVersion": "proposal-submission.v2",
        "summary": "Agent A creates shared.cpp",
        "changes": [{
            "change_id": "chg-agent-a",
            "operation": "create",
            "relative_path": "src/shared.cpp",
            "source_path": null,
            "proposed_content": "// from agent A"
        }]
    })";
    const std::string rawB = R"({
        "schemaVersion": "proposal-submission.v2",
        "summary": "Agent B also creates shared.cpp",
        "changes": [{
            "change_id": "chg-agent-b",
            "operation": "create",
            "relative_path": "src/shared.cpp",
            "source_path": null,
            "proposed_content": "// from agent B"
        }]
    })";

    auto reqA = makeServiceRequest(tmp.path / "project", rawA);
    reqA.runId = "run-parallel-1";
    const auto resultA = svc.createFromModelOutput(reqA);
    assert(resultA.success());
    assert(resultA.value().state == ChangeSetState::ReadyForReview);

    auto reqB = makeServiceRequest(tmp.path / "project", rawB);
    reqB.runId = "run-parallel-1";
    const auto resultB = svc.createFromModelOutput(reqB);
    assert(resultB.success()); // service succeeds but the proposal is Rejected
    assert(resultB.value().state == ChangeSetState::Rejected);
    bool foundConflict = false;
    for (const auto& f : resultB.value().validationFindings) {
        if (f.code == "proposal.cross_agent_conflict") {
            foundConflict = true;
            assert(f.changeId == "chg-agent-a");
            assert(f.message.find(resultA.value().changeSetId) != std::string::npos);
            break;
        }
    }
    assert(foundConflict);

    // Agent A's already-published proposal must be unaffected by B's failure.
    const auto refetchedA = svc.findChangeSet(resultA.value().changeSetId);
    assert(refetchedA.success());
    assert(refetchedA.value().state == ChangeSetState::ReadyForReview);
}

void testCrossAgentConflictDoesNotBlockDifferentPaths()
{
    TempDir tmp;
    FakeChangeProposalRepository repo;
    auto storeResult = FileChangeProposalStore::open(tmp.path / "proposals");
    assert(storeResult.success());
    RunPathClaimRegistry registry;
    ChangeProposalService svc(repo, *storeResult.value(), &registry);

    const std::string rawA = R"({
        "schemaVersion": "proposal-submission.v2",
        "summary": "Agent A creates a.cpp",
        "changes": [{
            "change_id": "chg-agent-a",
            "operation": "create",
            "relative_path": "src/a.cpp",
            "source_path": null,
            "proposed_content": "// a"
        }]
    })";
    const std::string rawB = R"({
        "schemaVersion": "proposal-submission.v2",
        "summary": "Agent B creates b.cpp",
        "changes": [{
            "change_id": "chg-agent-b",
            "operation": "create",
            "relative_path": "src/b.cpp",
            "source_path": null,
            "proposed_content": "// b"
        }]
    })";

    auto reqA = makeServiceRequest(tmp.path / "project", rawA);
    reqA.runId = "run-parallel-2";
    const auto resultA = svc.createFromModelOutput(reqA);
    auto reqB = makeServiceRequest(tmp.path / "project", rawB);
    reqB.runId = "run-parallel-2";
    const auto resultB = svc.createFromModelOutput(reqB);

    assert(resultA.success() && resultA.value().state == ChangeSetState::ReadyForReview);
    assert(resultB.success() && resultB.value().state == ChangeSetState::ReadyForReview);
}

// Documents the explicit opt-out: a caller that never wires a
// RunPathClaimRegistry (e.g. single-agent ReadOnlyAnalysisWorkflowRunner,
// where no other agent can ever race it) keeps today's behavior exactly -
// two same-path proposals in the same run both succeed independently.
void testNoRegistryMeansNoCrossAgentCheck()
{
    TempDir tmp;
    FakeChangeProposalRepository repo;
    auto storeResult = FileChangeProposalStore::open(tmp.path / "proposals");
    assert(storeResult.success());
    ChangeProposalService svc(repo, *storeResult.value()); // no registry

    const std::string rawA = R"({
        "schemaVersion": "proposal-submission.v2",
        "summary": "First proposal",
        "changes": [{
            "change_id": "chg-a",
            "operation": "create",
            "relative_path": "src/shared.cpp",
            "source_path": null,
            "proposed_content": "// first"
        }]
    })";

    auto reqA = makeServiceRequest(tmp.path / "project", rawA);
    reqA.runId = "run-no-registry";
    const auto resultA = svc.createFromModelOutput(reqA);
    assert(resultA.success() && resultA.value().state == ChangeSetState::ReadyForReview);

    // A second call for the same path in the same run is unaffected by any
    // cross-agent bookkeeping since none is wired - still limited only by
    // the real file-existence precondition, which now legitimately fires
    // because the first proposal was never applied to disk. This proves the
    // absence of the registry, not a false negative: rejecting here for
    // proposal.create_target_exists could only happen if the file actually
    // existed, which it never does in FakeChangeProposalRepository/
    // FileChangeProposalStore-only tests without an apply step.
    auto reqB = makeServiceRequest(tmp.path / "project", rawA);
    reqB.runId = "run-no-registry";
    const auto resultB = svc.createFromModelOutput(reqB);
    assert(resultB.success() && resultB.value().state == ChangeSetState::ReadyForReview);
}

void testServiceUnsafePathNeverSnapshotsOutsideRoot()
{
    TempDir tmp;
    FakeChangeProposalRepository repo;
    auto storeResult = FileChangeProposalStore::open(tmp.path / "proposals");
    assert(storeResult.success());
    ChangeProposalService svc(repo, *storeResult.value());

    const std::string outsideContent = "OUTSIDE_SENTINEL";
    tmp.writeFile("outside.cpp", outsideContent);
    const fs::path outsidePath = tmp.path / "outside.cpp";
    const std::string raw =
        std::string(R"({"schemaVersion":"proposal-submission.v2","summary":"Unsafe absolute path","changes":[{)")
        + R"("change_id":"chg-unsafe","operation":"modify","relative_path":")"
        + outsidePath.generic_string()
        + R"(","source_path":null,"proposed_content":"updated"}]})";

    auto req = makeServiceRequest(tmp.path / "project", raw);
    req.requireHashEvidence = true;
    req.observedFileHashes[detail::normalizedPathKey(outsidePath)] = sha256Hex(outsideContent);
    const auto result = svc.createFromModelOutput(req);
    if (!result.success()) {
        throw std::runtime_error(
            result.error()->code + ": " + result.error()->message);
    }
    assert(result.success());
    assert(result.value().state == ChangeSetState::Rejected);

    const auto leaked = storeResult.value()->readOriginalContent(
        result.value().artifactRelativeRoot, "chg-unsafe");
    assert(!leaked.success());
}

void testServiceSecretInOriginalSnapshotIsNotStored()
{
    TempDir tmp;
    FakeChangeProposalRepository repo;
    auto storeResult = FileChangeProposalStore::open(tmp.path / "proposals");
    assert(storeResult.success());
    ChangeProposalService svc(repo, *storeResult.value());

    const std::string original = "AKIAIOSFODNN7EXAMPLE12345678";
    tmp.writeFile("project/src/config.cpp", original);
    const std::string raw =
        std::string(R"({"schemaVersion":"proposal-submission.v2","summary":"Modify config","changes":[{)")
        + R"("change_id":"chg-original-secret","operation":"modify",)"
        + R"("relative_path":"src/config.cpp","source_path":null,"proposed_content":"clean replacement"}]})";

    auto req = makeServiceRequest(tmp.path / "project", raw);
    req.requireHashEvidence = true;
    req.observedFileHashes[detail::normalizedPathKey(fs::path("src/config.cpp"))] = sha256Hex(original);
    const auto result = svc.createFromModelOutput(req);
    assert(!result.success());
    assert(result.error()->code == "proposal.secret_detected");
    assert(repo.size() == 0);
    assert(!fs::exists(tmp.path / "proposals" / "runs"));
}

void testServiceSecretDetectedNothingStored()
{
    TempDir tmp;
    FakeChangeProposalRepository repo;
    auto storeResult = FileChangeProposalStore::open(tmp.path / "proposals");
    assert(storeResult.success());
    ChangeProposalService svc(repo, *storeResult.value());

    const std::string raw = R"({
        "schemaVersion": "proposal-submission.v2",
        "summary": "Add config",
        "changes": [{
            "change_id": "chg-sec-1",
            "operation": "create",
            "relative_path": "src/config.cpp",
            "source_path": null,
            "proposed_content": "AKIAIOSFODNN7EXAMPLE12345678"
        }]
    })";

    auto req = makeServiceRequest(tmp.path / "project", raw);
    const auto result = svc.createFromModelOutput(req);
    assert(!result.success());
    assert(result.error()->code == "proposal.secret_detected");
    // Nothing must be stored in the repository
    assert(repo.size() == 0);
    // No artifact files must have been written
    assert(!fs::exists(tmp.path / "proposals" / "runs"));
}

void testServiceMalformedJsonNothingStored()
{
    TempDir tmp;
    FakeChangeProposalRepository repo;
    auto storeResult = FileChangeProposalStore::open(tmp.path / "proposals");
    assert(storeResult.success());
    ChangeProposalService svc(repo, *storeResult.value());

    auto req = makeServiceRequest(tmp.path / "project", "{ invalid json !!!");
    const auto result = svc.createFromModelOutput(req);
    assert(!result.success());
    assert(result.error()->code == "proposal.parse_error");
    assert(repo.size() == 0);
}

void testServiceListAndFindQueries()
{
    TempDir tmp;
    FakeChangeProposalRepository repo;
    auto storeResult = FileChangeProposalStore::open(tmp.path / "proposals");
    assert(storeResult.success());
    ChangeProposalService svc(repo, *storeResult.value());

    const std::string raw = R"({
        "schemaVersion": "proposal-submission.v2",
        "summary": "Query test proposal",
        "changes": [{
            "change_id": "chg-q1",
            "operation": "create",
            "relative_path": "src/query.cpp",
            "source_path": null,
            "proposed_content": "// query"
        }]
    })";

    auto req = makeServiceRequest(tmp.path / "project", raw);
    req.runId = "run-query-001";
    req.projectId = "proj-query-001";
    const auto created = svc.createFromModelOutput(req);
    assert(created.success());
    const std::string csId = created.value().changeSetId;

    // listForRun
    const auto forRun = svc.listForRun("run-query-001");
    assert(forRun.success());
    assert(forRun.value().size() == 1);
    assert(forRun.value()[0].changeSetId == csId);

    // listForProject
    const auto forProject = svc.listForProject("proj-query-001");
    assert(forProject.success());
    assert(forProject.value().size() == 1);

    // findChangeSet
    const auto found = svc.findChangeSet(csId);
    assert(found.success());
    assert(found.value().changeSetId == csId);

    // findChangeSet not found
    const auto missing = svc.findChangeSet("no-such-id");
    assert(!missing.success());
    assert(missing.error()->code == "proposal.not_found");

    // listChanges
    const auto changes = svc.listChanges(csId);
    assert(changes.success());
    assert(changes.value().size() == 1);
    assert(changes.value()[0].changeId == "chg-q1");

    // listValidationFindings (empty for ReadyForReview)
    const auto findings = svc.listValidationFindings(csId);
    assert(findings.success());
    assert(findings.value().empty());
}

void testServiceReadContent()
{
    TempDir tmp;
    FakeChangeProposalRepository repo;
    auto storeResult = FileChangeProposalStore::open(tmp.path / "proposals");
    assert(storeResult.success());
    ChangeProposalService svc(repo, *storeResult.value());

    const std::string raw = R"({
        "schemaVersion": "proposal-submission.v2",
        "summary": "Content read test",
        "changes": [{
            "change_id": "chg-r1",
            "operation": "create",
            "relative_path": "src/read_test.cpp",
            "source_path": null,
            "proposed_content": "const char* greeting = \"hello\";"
        }]
    })";

    auto req = makeServiceRequest(tmp.path / "project", raw);
    const auto created = svc.createFromModelOutput(req);
    assert(created.success());
    const std::string csId = created.value().changeSetId;

    // Read proposed content through service (which looks up artifactRelativeRoot)
    const auto content = svc.readProposedContent(csId, "chg-r1");
    assert(content.success());
    assert(content.value() == "const char* greeting = \"hello\";");
}

// ---------------------------------------------------------------------------
// SQLite round-trip tests
// ---------------------------------------------------------------------------

void testSqliteChangeProposalRoundTrip()
{
    TempDir tmp;
    const fs::path dbPath = tmp.path / "state" / "test.sqlite3";
    fs::create_directories(dbPath.parent_path());

    // Open database — migration runs automatically (schema v20)
    auto dbResult = SqliteDatabase::open(dbPath);
    assert(dbResult.success());
    auto db = dbResult.value();

    SqliteChangeProposalRepository repo(db);

    // Build a change set with a Create and a finding
    ProposedFileChange ch;
    ch.changeId = "chg-sq1";
    ch.operation = FileChangeOperation::Create;
    ch.relativePath = "src/sq.cpp";
    ch.proposedContent = "int sq() {}";

    ValidationFinding vf;
    vf.code = "proposal.test_finding";
    vf.message = "SQLite finding test";
    vf.changeId = "chg-sq1";

    ProposedChangeSet cs;
    cs.schemaVersion = "proposal.v1";
    cs.changeSetId = "cs-sq-001";
    cs.runId = "run-sq-001";
    cs.projectId = "proj-sq-001";
    cs.providerId = "openai";
    cs.modelId = "gpt-4";
    cs.agentRole = "coder";
    cs.summary = "SQLite test proposal";
    cs.baseRevision = "main";
    cs.state = ChangeSetState::Validated;
    cs.artifactRelativeRoot = "runs/run-sq-001/proposals/cs-sq-001";
    cs.policySnapshotHash = "deadbeef";
    cs.createdAt = std::chrono::system_clock::now();
    cs.changes = {ch};
    cs.validationFindings = {vf};

    // Insert
    const auto insertResult = repo.insertChangeSet(cs);
    assert(insertResult.success());

    // Find
    const auto found = repo.findChangeSet("cs-sq-001");
    assert(found.success());
    assert(found.value().has_value());
    const auto& row = *found.value();
    assert(row.changeSetId == "cs-sq-001");
    assert(row.runId == "run-sq-001");
    assert(row.projectId == "proj-sq-001");
    assert(row.summary == "SQLite test proposal");
    assert(row.state == ChangeSetState::Validated);
    assert(row.artifactRelativeRoot == "runs/run-sq-001/proposals/cs-sq-001");

    // Update state
    const auto updated = repo.updateChangeSetState("cs-sq-001", ChangeSetState::ReadyForReview);
    assert(updated.success());
    assert(updated.value().state == ChangeSetState::ReadyForReview);

    // listForRun
    const auto forRun = repo.listForRun("run-sq-001");
    assert(forRun.success());
    assert(forRun.value().size() == 1);
    assert(forRun.value()[0].changeSetId == "cs-sq-001");

    // listForProject
    const auto forProject = repo.listForProject("proj-sq-001");
    assert(forProject.success());
    assert(forProject.value().size() == 1);

    // listChanges
    const auto changes = repo.listChanges("cs-sq-001");
    assert(changes.success());
    assert(changes.value().size() == 1);
    assert(changes.value()[0].changeId == "chg-sq1");
    assert(changes.value()[0].operation == FileChangeOperation::Create);
    // proposedContent NOT persisted in SQLite (stored in filesystem only)

    // listValidationFindings
    const auto findings = repo.listValidationFindings("cs-sq-001");
    assert(findings.success());
    assert(findings.value().size() == 1);
    assert(findings.value()[0].code == "proposal.test_finding");
    assert(findings.value()[0].changeId == "chg-sq1");

    // Duplicate insertion must fail
    const auto dup = repo.insertChangeSet(cs);
    assert(!dup.success());
    assert(dup.error()->code == "proposal.duplicate_id");

    // Not found
    const auto missing = repo.findChangeSet("no-such-cs");
    assert(missing.success());
    assert(!missing.value().has_value());
}

// Regression test: SqliteChangeProposalRepository read sourcePath back via
// the plain std::filesystem::path(std::string) constructor instead of the
// file's own pathFromUtf8() helper - a non-ASCII source_path (Rename)
// round-tripped through SQLite corrupted the same way a freshly-parsed
// proposal's relative_path did (see testParseNonAsciiRelativePathPreservesUtf8).
void testSqliteRenameSourcePathPreservesUtf8()
{
    TempDir tmp;
    const fs::path dbPath = tmp.path / "state" / "test.sqlite3";
    fs::create_directories(dbPath.parent_path());
    auto dbResult = SqliteDatabase::open(dbPath);
    assert(dbResult.success());
    SqliteChangeProposalRepository repo(dbResult.value());

    ProposedFileChange ch;
    ch.changeId = "chg-rename-utf8";
    ch.operation = FileChangeOperation::Rename;
    ch.relativePath = fs::path(u8"новое_имя.txt");
    ch.sourcePath = fs::path(u8"Тест/файл.txt");
    ch.expectedOriginalHash = std::string(64, 'a');

    ProposedChangeSet cs;
    cs.schemaVersion = "proposal.v1";
    cs.changeSetId = "cs-rename-utf8";
    cs.runId = "run-rename-utf8";
    cs.projectId = "proj-rename-utf8";
    cs.providerId = "openai";
    cs.modelId = "gpt-4";
    cs.agentRole = "coder";
    cs.summary = "Rename UTF-8 test";
    cs.state = ChangeSetState::ReadyForReview;
    cs.createdAt = std::chrono::system_clock::now();
    cs.changes = {ch};
    assert(repo.insertChangeSet(cs).success());

    const auto changes = repo.listChanges("cs-rename-utf8");
    assert(changes.success());
    assert(changes.value().size() == 1);
    assert(changes.value()[0].sourcePath.has_value());
    assert(changes.value()[0].sourcePath->generic_u8string()
        == std::u8string(u8"Тест/файл.txt"));
    assert(changes.value()[0].relativePath.generic_u8string()
        == std::u8string(u8"новое_имя.txt"));

    // The same fix applies to the batched read path.
    const auto batched = repo.listChangesForSets({"cs-rename-utf8"});
    assert(batched.success());
    assert(batched.value().at("cs-rename-utf8")[0].sourcePath.has_value());
    assert(batched.value().at("cs-rename-utf8")[0].sourcePath->generic_u8string()
        == std::u8string(u8"Тест/файл.txt"));
}

void testSqliteSchemaV20Migration()
{
    // Opening a fresh database should yield schema v20 with all three proposal
    // tables.  We verify by successfully inserting a minimal change set — if
    // the table were absent the insert would fail.
    TempDir tmp;
    const fs::path dbPath = tmp.path / "state" / "migration.sqlite3";
    fs::create_directories(dbPath.parent_path());

    auto dbResult = SqliteDatabase::open(dbPath);
    assert(dbResult.success());
    SqliteChangeProposalRepository repo(dbResult.value());

    ProposedChangeSet cs;
    cs.schemaVersion = "proposal.v1";
    cs.changeSetId = "cs-mig-001";
    cs.runId = "run-mig";
    cs.projectId = "proj-mig";
    cs.summary = "Migration test";
    cs.state = ChangeSetState::Received;
    cs.createdAt = std::chrono::system_clock::now();

    const auto result = repo.insertChangeSet(cs);
    assert(result.success());

    // Reopen the same database (simulates process restart)
    auto db2Result = SqliteDatabase::open(dbPath);
    assert(db2Result.success());
    SqliteChangeProposalRepository repo2(db2Result.value());

    const auto found = repo2.findChangeSet("cs-mig-001");
    assert(found.success());
    assert(found.value().has_value());
    assert(found.value()->runId == "run-mig");
}

// Phase 1 audit-evidence gate (REMAINING_GAPS_EXECUTION_PLAN.md): proves the
// schema v27 columns (model_execution_id actually populated,
// missing_evidence_code/message, supersedes_change_set_id) round-trip
// through a real SQLite connection, including a process-restart reopen -
// AgentToolTurnLoopRunnerTests.cpp covers the same contract end to end
// against the runner, but only via the in-memory FakeChangeProposalRepository.
void testChangeSetPersistsModelExecutionIdMissingEvidenceAndSupersession()
{
    TempDir tmp;
    const fs::path dbPath = tmp.path / "state" / "evidence.sqlite3";
    fs::create_directories(dbPath.parent_path());

    auto dbResult = SqliteDatabase::open(dbPath);
    assert(dbResult.success());
    SqliteChangeProposalRepository repo(dbResult.value());

    ProposedChangeSet first;
    first.schemaVersion = "proposal.v1";
    first.changeSetId = "cs-evidence-first";
    first.runId = "run-evidence";
    first.projectId = "proj-evidence";
    first.summary = "Nothing to do.";
    first.state = ChangeSetState::NoChangesNeeded;
    first.modelExecutionId = "exec-001";
    first.createdAt = std::chrono::system_clock::now();
    assert(repo.insertChangeSet(first).success());

    // Not yet annotated: a plain NoChangesNeeded row is indistinguishable
    // from a genuinely-inspected one until markMissingEvidence() runs.
    {
        const auto reread = repo.findChangeSet("cs-evidence-first");
        assert(reread.success() && reread.value().has_value());
        assert(reread.value()->modelExecutionId.has_value());
        assert(*reread.value()->modelExecutionId == "exec-001");
        assert(!reread.value()->missingEvidenceCode.has_value());
        assert(!reread.value()->supersedesChangeSetId.has_value());
    }

    const auto marked = repo.markMissingEvidence(
        "cs-evidence-first", "proposal.no_changes_missing_evidence",
        "No workspace inspection tool completed successfully");
    assert(marked.success());

    const auto missing = repo.markMissingEvidence(
        "cs-does-not-exist", "proposal.no_changes_missing_evidence", "n/a");
    assert(!missing.success());
    assert(missing.error()->code == "proposal.not_found");

    ProposedChangeSet second;
    second.schemaVersion = "proposal.v1";
    second.changeSetId = "cs-evidence-second";
    second.runId = "run-evidence";
    second.projectId = "proj-evidence";
    second.summary = "Still nothing to do, after repair.";
    second.state = ChangeSetState::NoChangesNeeded;
    second.modelExecutionId = "exec-001";
    second.supersedesChangeSetId = "cs-evidence-first";
    second.createdAt = std::chrono::system_clock::now();
    assert(repo.insertChangeSet(second).success());

    // Reopen the same database (simulates process restart) before the final
    // assertions, so this proves durable persistence, not an in-process cache.
    auto db2Result = SqliteDatabase::open(dbPath);
    assert(db2Result.success());
    SqliteChangeProposalRepository repo2(db2Result.value());

    const auto rereadFirst = repo2.findChangeSet("cs-evidence-first");
    assert(rereadFirst.success() && rereadFirst.value().has_value());
    assert(rereadFirst.value()->missingEvidenceCode.has_value());
    assert(*rereadFirst.value()->missingEvidenceCode == "proposal.no_changes_missing_evidence");
    assert(rereadFirst.value()->missingEvidenceMessage.has_value());
    assert(rereadFirst.value()->state == ChangeSetState::NoChangesNeeded);

    const auto rereadSecond = repo2.findChangeSet("cs-evidence-second");
    assert(rereadSecond.success() && rereadSecond.value().has_value());
    assert(rereadSecond.value()->supersedesChangeSetId.has_value());
    assert(*rereadSecond.value()->supersedesChangeSetId == "cs-evidence-first");
}

// ---------------------------------------------------------------------------
// Batched reads: SqliteChangeProposalRepository::listChangesForSets() /
// listValidationFindingsForSets()
// ---------------------------------------------------------------------------

SqliteChangeProposalRepository openBatchTestRepo(const fs::path& dbPath)
{
    fs::create_directories(dbPath.parent_path());
    auto dbResult = SqliteDatabase::open(dbPath);
    assert(dbResult.success());
    return SqliteChangeProposalRepository(dbResult.value());
}

ProposedChangeSet makeBatchTestChangeSet(
    const std::string& changeSetId,
    const std::string& runId,
    std::vector<ProposedFileChange> changes,
    std::vector<ValidationFinding> findings = {})
{
    ProposedChangeSet cs;
    cs.schemaVersion = "proposal.v1";
    cs.changeSetId = changeSetId;
    cs.runId = runId;
    cs.projectId = "proj-batch";
    cs.providerId = "openai";
    cs.modelId = "gpt-4";
    cs.agentRole = "coder";
    cs.summary = "Batch read test " + changeSetId;
    cs.state = ChangeSetState::ReadyForReview;
    cs.createdAt = std::chrono::system_clock::now();
    cs.changes = std::move(changes);
    cs.validationFindings = std::move(findings);
    return cs;
}

void testBatchedReadsEmptyInputReturnsEmptySuccess()
{
    TempDir tmp;
    auto repo = openBatchTestRepo(tmp.path / "state" / "test.sqlite3");

    const auto changes = repo.listChangesForSets({});
    assert(changes.success());
    assert(changes.value().empty());

    const auto findings = repo.listValidationFindingsForSets({});
    assert(findings.success());
    assert(findings.value().empty());
}

void testBatchedReadsSingleChangeSet()
{
    TempDir tmp;
    auto repo = openBatchTestRepo(tmp.path / "state" / "test.sqlite3");

    ProposedFileChange ch;
    ch.changeId = "c1";
    ch.operation = FileChangeOperation::Modify;
    ch.relativePath = "src/single.cpp";
    ch.expectedOriginalHash = std::string(64, 'a');
    ValidationFinding f;
    f.code = "proposal.test";
    f.message = "single-set test";
    f.changeId = "c1";
    const auto cs = makeBatchTestChangeSet("cs-single", "run-single", {ch}, {f});
    assert(repo.insertChangeSet(cs).success());

    const auto changes = repo.listChangesForSets({"cs-single"});
    assert(changes.success());
    assert(changes.value().size() == 1);
    assert(changes.value().at("cs-single").size() == 1);
    assert(changes.value().at("cs-single")[0].changeId == "c1");
    assert(changes.value().at("cs-single")[0].operation == FileChangeOperation::Modify);

    const auto findings = repo.listValidationFindingsForSets({"cs-single"});
    assert(findings.success());
    assert(findings.value().size() == 1);
    assert(findings.value().at("cs-single").size() == 1);
    assert(findings.value().at("cs-single")[0].code == "proposal.test");
}

void testBatchedReadsMultipleChangeSetsDoNotCrossContaminate()
{
    TempDir tmp;
    auto repo = openBatchTestRepo(tmp.path / "state" / "test.sqlite3");

    ProposedFileChange chA;
    chA.changeId = "a1";
    chA.operation = FileChangeOperation::Create;
    chA.relativePath = "src/a.cpp";
    ProposedFileChange chB1;
    chB1.changeId = "b1";
    chB1.operation = FileChangeOperation::Create;
    chB1.relativePath = "src/b1.cpp";
    ProposedFileChange chB2;
    chB2.changeId = "b2";
    chB2.operation = FileChangeOperation::Create;
    chB2.relativePath = "src/b2.cpp";
    const auto csA = makeBatchTestChangeSet("cs-a", "run-multi", {chA});
    const auto csB = makeBatchTestChangeSet("cs-b", "run-multi", {chB1, chB2});
    assert(repo.insertChangeSet(csA).success());
    assert(repo.insertChangeSet(csB).success());

    const auto changes = repo.listChangesForSets({"cs-a", "cs-b"});
    assert(changes.success());
    assert(changes.value().size() == 2);
    assert(changes.value().at("cs-a").size() == 1);
    assert(changes.value().at("cs-a")[0].changeId == "a1");
    assert(changes.value().at("cs-b").size() == 2);
    assert(changes.value().at("cs-b")[0].changeId == "b1");
    assert(changes.value().at("cs-b")[1].changeId == "b2");
}

void testBatchedReadsMissingIdIsNotAnError()
{
    TempDir tmp;
    auto repo = openBatchTestRepo(tmp.path / "state" / "test.sqlite3");

    ProposedFileChange ch;
    ch.changeId = "c1";
    ch.operation = FileChangeOperation::Create;
    ch.relativePath = "src/a.cpp";
    const auto cs = makeBatchTestChangeSet("cs-real", "run-missing", {ch});
    assert(repo.insertChangeSet(cs).success());

    const auto result = repo.listChangesForSets({"cs-real", "cs-does-not-exist"});
    assert(result.success());
    assert(result.value().size() == 1);
    assert(result.value().count("cs-does-not-exist") == 0);
    assert(result.value().at("cs-real").size() == 1);
}

void testBatchedReadsDuplicateIdsProduceNoDuplicateResults()
{
    TempDir tmp;
    auto repo = openBatchTestRepo(tmp.path / "state" / "test.sqlite3");

    ProposedFileChange ch1;
    ch1.changeId = "c1";
    ch1.operation = FileChangeOperation::Create;
    ch1.relativePath = "src/a.cpp";
    ProposedFileChange ch2;
    ch2.changeId = "c2";
    ch2.operation = FileChangeOperation::Create;
    ch2.relativePath = "src/b.cpp";
    const auto cs = makeBatchTestChangeSet("cs-dup", "run-dup", {ch1, ch2});
    assert(repo.insertChangeSet(cs).success());

    const auto changes = repo.listChangesForSets({"cs-dup", "cs-dup", "cs-dup"});
    assert(changes.success());
    assert(changes.value().size() == 1);
    assert(changes.value().at("cs-dup").size() == 2);
}

void testBatchedReadsPreserveOrdinalOrderWithinChangeSet()
{
    TempDir tmp;
    auto repo = openBatchTestRepo(tmp.path / "state" / "test.sqlite3");

    std::vector<ProposedFileChange> changes;
    for (int i = 0; i < 5; ++i) {
        ProposedFileChange ch;
        ch.changeId = "c" + std::to_string(i);
        ch.operation = FileChangeOperation::Create;
        ch.relativePath = "src/f" + std::to_string(i) + ".cpp";
        changes.push_back(ch);
    }
    std::vector<ValidationFinding> findings;
    for (int i = 0; i < 3; ++i) {
        ValidationFinding f;
        f.code = "finding-" + std::to_string(i);
        f.message = "msg " + std::to_string(i);
        f.changeId = "c0";
        findings.push_back(f);
    }
    const auto cs = makeBatchTestChangeSet("cs-order", "run-order", changes, findings);
    assert(repo.insertChangeSet(cs).success());

    const auto changesResult = repo.listChangesForSets({"cs-order"});
    assert(changesResult.success());
    const auto& orderedChanges = changesResult.value().at("cs-order");
    assert(orderedChanges.size() == 5);
    for (std::size_t i = 0; i < 5; ++i)
        assert(orderedChanges[i].changeId == "c" + std::to_string(i));

    const auto findingsResult = repo.listValidationFindingsForSets({"cs-order"});
    assert(findingsResult.success());
    const auto& orderedFindings = findingsResult.value().at("cs-order");
    assert(orderedFindings.size() == 3);
    for (std::size_t i = 0; i < 3; ++i)
        assert(orderedFindings[i].code == "finding-" + std::to_string(i));
}

void testBatchedReadsChunkLargeIdListAcrossParameterLimit()
{
    TempDir tmp;
    auto repo = openBatchTestRepo(tmp.path / "state" / "test.sqlite3");

    // A handful of real change sets placed near the start, middle and end of
    // a requested-id list well over one batching chunk (500 ids - see
    // kBatchedInQueryChunkSize in
    // sqlite/proposals/SqliteChangeProposalRepository.cpp) - if chunking
    // dropped or misrouted a chunk, one of these would come back missing.
    const std::vector<std::size_t> realPositions = {0, 300, 650, 1199};
    for (std::size_t position : realPositions) {
        ProposedFileChange ch;
        ch.changeId = "chg-real-" + std::to_string(position);
        ch.operation = FileChangeOperation::Create;
        ch.relativePath = "src/real" + std::to_string(position) + ".cpp";
        const auto cs = makeBatchTestChangeSet(
            "cs-real-" + std::to_string(position), "run-chunk", {ch});
        assert(repo.insertChangeSet(cs).success());
    }

    std::vector<std::string> requestedIds;
    requestedIds.reserve(1200);
    for (std::size_t i = 0; i < 1200; ++i) {
        const bool isReal = std::find(realPositions.begin(), realPositions.end(), i)
            != realPositions.end();
        requestedIds.push_back(isReal
            ? ("cs-real-" + std::to_string(i))
            : ("cs-missing-" + std::to_string(i)));
    }

    const auto changes = repo.listChangesForSets(requestedIds);
    assert(changes.success());
    assert(changes.value().size() == realPositions.size());
    for (std::size_t position : realPositions) {
        const auto it = changes.value().find("cs-real-" + std::to_string(position));
        assert(it != changes.value().end());
        assert(it->second.size() == 1);
        assert(it->second[0].changeId == "chg-real-" + std::to_string(position));
    }
}

void testBatchedReadsSurfaceStatementFailureWithoutPartialSuccess()
{
    TempDir tmp;
    const fs::path dbPath = tmp.path / "state" / "test.sqlite3";
    fs::create_directories(dbPath.parent_path());

    // Create the schema through a normal open/close first, then drop the
    // table the batched changes query depends on via a second raw
    // connection to the same file - the fault-injection technique already
    // established in SqliteRepositoryTests.cpp. The subsequently-reopened
    // repository connection must then fail cleanly (prepare() fails because
    // the table is gone), never return an empty/partial result as success.
    {
        auto dbResult = SqliteDatabase::open(dbPath);
        assert(dbResult.success());
    }
    {
        sqlite3* handle = nullptr;
        const std::u8string path = dbPath.generic_u8string();
        const std::string utf8Path(reinterpret_cast<const char*>(path.data()), path.size());
        assert(sqlite3_open_v2(utf8Path.c_str(), &handle, SQLITE_OPEN_READWRITE, nullptr) == SQLITE_OK);
        assert(sqlite3_exec(handle, "DROP TABLE proposed_file_changes;", nullptr, nullptr, nullptr)
            == SQLITE_OK);
        sqlite3_close_v2(handle);
    }

    auto dbResult = SqliteDatabase::open(dbPath);
    assert(dbResult.success());
    SqliteChangeProposalRepository repo(dbResult.value());

    const auto result = repo.listChangesForSets({"any-id"});
    assert(!result.success());
    assert(result.error()->code == "persistence.prepare_failed");
}

// ---------------------------------------------------------------------------
// Approval audit trail: SqliteChangeApprovalRepository (schema v25)
// ---------------------------------------------------------------------------

void testSqliteChangeApprovalRoundTrip()
{
    TempDir tmp;
    const fs::path dbPath = tmp.path / "state" / "test.sqlite3";
    fs::create_directories(dbPath.parent_path());

    auto dbResult = SqliteDatabase::open(dbPath);
    assert(dbResult.success());
    auto db = dbResult.value();

    SqliteChangeProposalRepository proposals(db);
    SqliteChangeApprovalRepository approvals(db);

    ProposedFileChange ch;
    ch.changeId = "chg-approval-1";
    ch.operation = FileChangeOperation::Create;
    ch.relativePath = "src/approval.cpp";

    ProposedChangeSet cs;
    cs.schemaVersion = "proposal.v1";
    cs.changeSetId = "cs-approval-001";
    cs.runId = "run-approval-001";
    cs.projectId = "proj-approval-001";
    cs.providerId = "openai";
    cs.modelId = "gpt-4";
    cs.agentRole = "coder";
    cs.summary = "Approval round-trip test";
    cs.state = ChangeSetState::ReadyForReview;
    cs.createdAt = std::chrono::system_clock::now();
    cs.changes = {ch};
    assert(proposals.insertChangeSet(cs).success());

    // No approval yet.
    const auto missing = approvals.findApproval(cs.changeSetId);
    assert(missing.success());
    assert(!missing.value().has_value());

    ApprovalRecord record;
    record.changeSetId = cs.changeSetId;
    record.snapshotHash = "deadbeefcafe";
    record.approvedAt = std::chrono::system_clock::now();
    record.approvedBy = "test-user";
    assert(approvals.recordApproval(record).success());

    // Duplicate insert must fail.
    const auto dup = approvals.recordApproval(record);
    assert(!dup.success());
    assert(dup.error()->code == "proposal.duplicate_id");

    const auto found = approvals.findApproval(cs.changeSetId);
    assert(found.success());
    assert(found.value().has_value());
    assert(found.value()->snapshotHash == "deadbeefcafe");
    assert(found.value()->approvedBy.has_value() && *found.value()->approvedBy == "test-user");
    assert(!found.value()->appliedAt.has_value());
    assert(!found.value()->applyResult.has_value());
    assert(found.value()->applyHashLog.empty());

    // recordApplyOutcome updates the same row.
    std::vector<ApplyHashEntry> hashLog;
    ApplyHashEntry entry;
    entry.changeId = "chg-approval-1";
    entry.relativePath = "src/approval.cpp";
    entry.newHash = "newcontenthash";
    hashLog.push_back(entry);
    assert(approvals.recordApplyOutcome(
        cs.changeSetId, "applied", std::nullopt, hashLog,
        std::optional<std::string>("post-apply-fingerprint")).success());

    const auto afterApply = approvals.findApproval(cs.changeSetId);
    assert(afterApply.success());
    assert(afterApply.value().has_value());
    assert(afterApply.value()->appliedAt.has_value());
    assert(afterApply.value()->applyResult.has_value()
        && *afterApply.value()->applyResult == "applied");
    assert(!afterApply.value()->applyErrorCode.has_value());
    assert(afterApply.value()->applyHashLog.size() == 1);
    assert(afterApply.value()->applyHashLog[0].changeId == "chg-approval-1");
    assert(!afterApply.value()->applyHashLog[0].originalHash.has_value());
    assert(afterApply.value()->applyHashLog[0].newHash.has_value()
        && *afterApply.value()->applyHashLog[0].newHash == "newcontenthash");
    assert(afterApply.value()->postApplyWorkspaceFingerprint.has_value()
        && *afterApply.value()->postApplyWorkspaceFingerprint == "post-apply-fingerprint");

    // recordApplyOutcome for an unknown change set fails.
    const auto notFound = approvals.recordApplyOutcome(
        "no-such-cs", "applied", std::nullopt, {}, std::nullopt);
    assert(!notFound.success());
    assert(notFound.error()->code == "proposal.not_found");
}

void testSqliteSchemaV25Migration()
{
    TempDir tmp;
    const fs::path dbPath = tmp.path / "state" / "migration25.sqlite3";
    fs::create_directories(dbPath.parent_path());

    auto dbResult = SqliteDatabase::open(dbPath);
    assert(dbResult.success());
    SqliteChangeProposalRepository proposals(dbResult.value());
    SqliteChangeApprovalRepository approvals(dbResult.value());

    ProposedChangeSet cs;
    cs.schemaVersion = "proposal.v1";
    cs.changeSetId = "cs-mig25-001";
    cs.runId = "run-mig25";
    cs.projectId = "proj-mig25";
    cs.summary = "Migration v25 test";
    cs.state = ChangeSetState::ReadyForReview;
    cs.createdAt = std::chrono::system_clock::now();
    assert(proposals.insertChangeSet(cs).success());

    ApprovalRecord record;
    record.changeSetId = cs.changeSetId;
    record.snapshotHash = "hash";
    record.approvedAt = std::chrono::system_clock::now();
    assert(approvals.recordApproval(record).success());

    // Reopen the same database (simulates process restart) - the table must
    // still exist and the row must still be readable.
    auto db2Result = SqliteDatabase::open(dbPath);
    assert(db2Result.success());
    SqliteChangeApprovalRepository approvals2(db2Result.value());
    const auto found = approvals2.findApproval(cs.changeSetId);
    assert(found.success());
    assert(found.value().has_value());
    assert(found.value()->snapshotHash == "hash");
}

void testValidateExactWriteAllocationDoesNotAuthorizeDescendants()
{
    auto exact = makeSimpleCreate("chg-1", "src/component.qml");
    ProjectWritePolicy policy;
    policy.allowedRoots = {fs::path("src/component.qml")};
    policy.allowedRootsAreExactPaths = true;
    auto findings = ChangeProposalValidator::validate(exact, policy, {});
    assert(std::none_of(findings.begin(), findings.end(), [](const auto& finding) {
        return finding.code == "proposal.outside_allowed_roots";
    }));

    auto child = makeSimpleCreate("chg-2", "src/component.qml/escape.txt");
    findings = ChangeProposalValidator::validate(child, policy, {});
    assert(std::any_of(findings.begin(), findings.end(), [](const auto& finding) {
        return finding.code == "proposal.outside_allowed_roots";
    }));
}

void testSqliteSchemaV36AddsPostApplyFingerprintColumn()
{
    TempDir tmp;
    const fs::path dbPath = tmp.path / "state" / "migration36.sqlite3";
    fs::create_directories(dbPath.parent_path());
    {
        const auto opened = SqliteDatabase::open(dbPath);
        assert(opened.success());
        SqliteChangeProposalRepository proposals(opened.value());
        SqliteChangeApprovalRepository approvals(opened.value());
        ProposedChangeSet cs;
        cs.schemaVersion = "proposal.v1";
        cs.changeSetId = "cs-mig36-001";
        cs.runId = "run-mig36";
        cs.projectId = "proj-mig36";
        cs.summary = "Migration v36 test";
        cs.state = ChangeSetState::ReadyForReview;
        cs.createdAt = std::chrono::system_clock::now();
        assert(proposals.insertChangeSet(cs).success());
        ApprovalRecord approval;
        approval.changeSetId = cs.changeSetId;
        approval.snapshotHash = "hash";
        approval.approvedAt = std::chrono::system_clock::now();
        assert(approvals.recordApproval(approval).success());
    }
    {
        sqlite3* raw = nullptr;
        assert(sqlite3_open(dbPath.string().c_str(), &raw) == SQLITE_OK);
        assert(sqlite3_exec(raw,
            "ALTER TABLE change_set_approvals "
            "DROP COLUMN post_apply_workspace_fingerprint;"
            "PRAGMA user_version = 35;",
            nullptr, nullptr, nullptr) == SQLITE_OK);
        sqlite3_close(raw);
    }
    const auto reopened = SqliteDatabase::open(dbPath);
    assert(reopened.success());
    SqliteChangeApprovalRepository approvals(reopened.value());
    assert(approvals.recordApplyOutcome(
        "cs-mig36-001", "applied", std::nullopt, {},
        std::optional<std::string>("restored-fingerprint")).success());
    const auto found = approvals.findApproval("cs-mig36-001");
    assert(found.success() && found.value().has_value());
    assert(found.value()->postApplyWorkspaceFingerprint
        == std::optional<std::string>("restored-fingerprint"));
}

// ---------------------------------------------------------------------------
// Proposal review (AG-07): durable human reject/discard, kept separate from
// ChangeSetState and from ApprovalRecord - see ProposalReviewTypes.hpp.
// ---------------------------------------------------------------------------

ProposedChangeSet makeReadyForReviewChangeSet(const std::string& id = "cs-review-001")
{
    ProposedChangeSet cs;
    cs.schemaVersion = "proposal.v1";
    cs.changeSetId = id;
    cs.runId = "run-review-001";
    cs.projectId = "proj-review-001";
    cs.providerId = "openai";
    cs.modelId = "gpt-4";
    cs.agentRole = "coder";
    cs.summary = "Reviewable proposal";
    cs.state = ChangeSetState::ReadyForReview;
    cs.createdAt = std::chrono::system_clock::now();
    return cs;
}

void testProposalReviewRejectSucceedsFromReadyForReview()
{
    FakeChangeProposalRepository proposals;
    FakeProposalReviewRepository reviews;
    ProposalReviewService service(proposals, reviews);

    auto cs = makeReadyForReviewChangeSet();
    assert(proposals.insertChangeSet(cs).success());

    const auto result = service.reject(
        cs.changeSetId, std::string("reviewer1"), std::string("Not needed"));
    assert(result.success());
    assert(result.value().changeSetId == cs.changeSetId);
    assert(result.value().rejectedBy.has_value() && *result.value().rejectedBy == "reviewer1");
    assert(result.value().reason.has_value() && *result.value().reason == "Not needed");

    // AG-07: the change set's own validator state is never relabeled.
    const auto found = proposals.findChangeSet(cs.changeSetId);
    assert(found.success() && found.value().has_value());
    assert(found.value()->state == ChangeSetState::ReadyForReview);
}

// PC-4C: only the first, genuine rejection nudges the Outcome Brief - not a replay, not a refusal.
void testProposalReviewRejectNudgesTheOutcomeBriefOnce()
{
    const auto must = [](bool condition, const char* message) {
        if (!condition) throw std::runtime_error(message);
    };
    FakeChangeProposalRepository proposals;
    FakeProposalReviewRepository reviews;
    outcomebrief_hooks::RecordingTrigger trigger;
    ProposalReviewService service(proposals, reviews);
    service.setOutcomeBriefTrigger(&trigger);

    auto ready = makeReadyForReviewChangeSet("cs-nudge-1");
    auto applied = makeReadyForReviewChangeSet("cs-nudge-2");
    applied.state = ChangeSetState::Applied;
    must(proposals.insertChangeSet(ready).success() && proposals.insertChangeSet(applied).success(), "setup");

    must(!service.reject("no-such-cs", std::nullopt, std::nullopt).success(), "setup: an unknown change set is refused");
    must(!service.reject(applied.changeSetId, std::nullopt, std::nullopt).success(), "setup: an applied change set is refused");
    must(trigger.proposals().empty(), "a refusal nudges nothing");

    must(service.reject(ready.changeSetId, std::string("reviewer"), std::string("no")).success(), "the first rejection");
    must(trigger.proposals() == std::vector<std::string> {ready.changeSetId}, "the genuine rejection nudges once");
    must(service.reject(ready.changeSetId, std::string("other"), std::string("again")).success(), "a replay is not an error");
    must(trigger.proposals().size() == 1 && trigger.runs().empty(), "...and a replay nudges nothing");
}

void testProposalReviewRejectFailsWhenNotReadyForReview()
{
    FakeChangeProposalRepository proposals;
    FakeProposalReviewRepository reviews;
    ProposalReviewService service(proposals, reviews);

    auto cs = makeReadyForReviewChangeSet("cs-review-002");
    cs.state = ChangeSetState::Applied;
    assert(proposals.insertChangeSet(cs).success());

    const auto result = service.reject(cs.changeSetId, std::nullopt, std::nullopt);
    assert(!result.success());
    assert(result.error()->code == "proposal.not_ready_for_review");
}

void testProposalReviewRejectFailsForUnknownChangeSet()
{
    FakeChangeProposalRepository proposals;
    FakeProposalReviewRepository reviews;
    ProposalReviewService service(proposals, reviews);

    const auto result = service.reject("no-such-cs", std::nullopt, std::nullopt);
    assert(!result.success());
    assert(result.error()->code == "proposal.not_found");
}

void testProposalReviewRejectIsIdempotent()
{
    FakeChangeProposalRepository proposals;
    FakeProposalReviewRepository reviews;
    ProposalReviewService service(proposals, reviews);

    auto cs = makeReadyForReviewChangeSet("cs-review-004");
    assert(proposals.insertChangeSet(cs).success());

    const auto first = service.reject(
        cs.changeSetId, std::string("reviewer1"), std::string("first reason"));
    assert(first.success());

    // A repeated reject command (double click, retried UI call) is not an
    // error - it returns the existing decision, unmodified by the new args.
    const auto second = service.reject(
        cs.changeSetId, std::string("reviewer2"), std::string("different reason"));
    assert(second.success());
    assert(second.value().rejectedBy.has_value() && *second.value().rejectedBy == "reviewer1");
    assert(second.value().reason.has_value() && *second.value().reason == "first reason");
}

void testProposalReviewRejectReasonTooLarge()
{
    FakeChangeProposalRepository proposals;
    FakeProposalReviewRepository reviews;
    ProposalReviewService service(proposals, reviews);

    auto cs = makeReadyForReviewChangeSet("cs-review-005");
    assert(proposals.insertChangeSet(cs).success());

    const std::string oversized(ProposalReviewService::kMaximumReasonBytes + 1, 'x');
    const auto result = service.reject(cs.changeSetId, std::nullopt, oversized);
    assert(!result.success());
    assert(result.error()->code == "proposal.review_reason_too_large");

    const auto found = reviews.findRejection(cs.changeSetId);
    assert(found.success() && !found.value().has_value());
}

void testProposalReviewAssertNotRejectedPassesWhenClean()
{
    FakeChangeProposalRepository proposals;
    FakeProposalReviewRepository reviews;
    ProposalReviewService service(proposals, reviews);

    auto cs = makeReadyForReviewChangeSet("cs-review-006");
    assert(proposals.insertChangeSet(cs).success());

    assert(service.assertNotRejected(cs.changeSetId).success());
}

void testProposalReviewAssertNotRejectedFailsAfterRejection()
{
    FakeChangeProposalRepository proposals;
    FakeProposalReviewRepository reviews;
    ProposalReviewService service(proposals, reviews);

    auto cs = makeReadyForReviewChangeSet("cs-review-007");
    assert(proposals.insertChangeSet(cs).success());
    assert(service.reject(cs.changeSetId, std::nullopt, std::nullopt).success());

    // This is what AgentSessionController::approveAndApply()/applyProposal()
    // call before ever reaching ChangeSetApplyService.
    const auto result = service.assertNotRejected(cs.changeSetId);
    assert(!result.success());
    assert(result.error()->code == "proposal.rejected_by_reviewer");
}

void testSqliteProposalReviewRoundTrip()
{
    TempDir tmp;
    const fs::path dbPath = tmp.path / "state" / "test.sqlite3";
    fs::create_directories(dbPath.parent_path());

    auto dbResult = SqliteDatabase::open(dbPath);
    assert(dbResult.success());
    auto db = dbResult.value();

    SqliteChangeProposalRepository proposals(db);
    SqliteProposalReviewRepository reviews(db);

    ProposedChangeSet cs;
    cs.schemaVersion = "proposal.v1";
    cs.changeSetId = "cs-reviewreject-001";
    cs.runId = "run-reviewreject-001";
    cs.projectId = "proj-reviewreject-001";
    cs.providerId = "openai";
    cs.modelId = "gpt-4";
    cs.agentRole = "coder";
    cs.summary = "Reviewer-rejection round-trip test";
    cs.state = ChangeSetState::ReadyForReview;
    cs.createdAt = std::chrono::system_clock::now();
    assert(proposals.insertChangeSet(cs).success());

    // No rejection yet.
    const auto missing = reviews.findRejection(cs.changeSetId);
    assert(missing.success());
    assert(!missing.value().has_value());

    ProposalReviewRejection record;
    record.changeSetId = cs.changeSetId;
    record.rejectedAt = std::chrono::system_clock::now();
    record.rejectedBy = "test-reviewer";
    record.reason = "Needs a different approach";
    assert(reviews.recordRejection(record).success());

    // Duplicate insert must fail.
    const auto dup = reviews.recordRejection(record);
    assert(!dup.success());
    assert(dup.error()->code == "proposal.duplicate_id");

    const auto found = reviews.findRejection(cs.changeSetId);
    assert(found.success());
    assert(found.value().has_value());
    assert(found.value()->rejectedBy.has_value() && *found.value()->rejectedBy == "test-reviewer");
    assert(found.value()->reason.has_value()
        && *found.value()->reason == "Needs a different approach");

    // The change set's own persisted state is untouched by a reviewer
    // rejection - separate facts, per ProposalReviewTypes.hpp.
    const auto csAfter = proposals.findChangeSet(cs.changeSetId);
    assert(csAfter.success() && csAfter.value().has_value());
    assert(csAfter.value()->state == ChangeSetState::ReadyForReview);
}

void testSqliteProposalReviewRejectionWithoutReasonRoundTrips()
{
    TempDir tmp;
    const fs::path dbPath = tmp.path / "state" / "test.sqlite3";
    fs::create_directories(dbPath.parent_path());

    auto dbResult = SqliteDatabase::open(dbPath);
    assert(dbResult.success());
    auto db = dbResult.value();

    SqliteChangeProposalRepository proposals(db);
    SqliteProposalReviewRepository reviews(db);

    ProposedChangeSet cs;
    cs.schemaVersion = "proposal.v1";
    cs.changeSetId = "cs-reviewreject-002";
    cs.runId = "run-reviewreject-002";
    cs.projectId = "proj-reviewreject-002";
    cs.summary = "No-reason rejection test";
    cs.state = ChangeSetState::ReadyForReview;
    cs.createdAt = std::chrono::system_clock::now();
    assert(proposals.insertChangeSet(cs).success());

    ProposalReviewRejection record;
    record.changeSetId = cs.changeSetId;
    record.rejectedAt = std::chrono::system_clock::now();
    // rejectedBy/reason both intentionally absent.
    assert(reviews.recordRejection(record).success());

    const auto found = reviews.findRejection(cs.changeSetId);
    assert(found.success() && found.value().has_value());
    assert(!found.value()->rejectedBy.has_value());
    assert(!found.value()->reason.has_value());
}

void testSqliteSchemaV30Migration()
{
    TempDir tmp;
    const fs::path dbPath = tmp.path / "state" / "migration30.sqlite3";
    fs::create_directories(dbPath.parent_path());

    auto dbResult = SqliteDatabase::open(dbPath);
    assert(dbResult.success());
    SqliteChangeProposalRepository proposals(dbResult.value());
    SqliteProposalReviewRepository reviews(dbResult.value());

    ProposedChangeSet cs;
    cs.schemaVersion = "proposal.v1";
    cs.changeSetId = "cs-mig30-001";
    cs.runId = "run-mig30";
    cs.projectId = "proj-mig30";
    cs.summary = "Migration v30 test";
    cs.state = ChangeSetState::ReadyForReview;
    cs.createdAt = std::chrono::system_clock::now();
    assert(proposals.insertChangeSet(cs).success());

    ProposalReviewRejection record;
    record.changeSetId = cs.changeSetId;
    record.rejectedAt = std::chrono::system_clock::now();
    assert(reviews.recordRejection(record).success());

    // Reopen the same database (simulates process restart) - the table must
    // still exist and the row must still be readable.
    auto db2Result = SqliteDatabase::open(dbPath);
    assert(db2Result.success());
    SqliteProposalReviewRepository reviews2(db2Result.value());
    const auto found = reviews2.findRejection(cs.changeSetId);
    assert(found.success());
    assert(found.value().has_value());
}

// ---------------------------------------------------------------------------
// Lineage-aware verification-repair materialization
// (ChangeProposalService::materializeRepairSuccessor)
// ---------------------------------------------------------------------------

MaterializeRepairSuccessorRequest makeMaterializeRequest(
    const fs::path& projectRoot,
    const ChangeSetId& predecessorId,
    const std::string& repairRawJson,
    bool allowDelete = false,
    bool allowRename = false)
{
    MaterializeRepairSuccessorRequest req;
    req.runId = "run-svc-001";
    req.projectId = "proj-svc-001";
    req.providerId = "openai";
    req.modelId = "gpt-4";
    req.agentRole = "code-agent";
    req.predecessorChangeSetId = predecessorId;
    req.repairRawModelOutput = repairRawJson;
    req.policy.projectRoot = projectRoot;
    req.policy.allowDelete = allowDelete;
    req.policy.allowRename = allowRename;
    req.baseRevision = "main";
    return req;
}

// The real Stage 2A incident this whole package closes: five validated
// Create changes, matching Run aaf6cded-4bd3-41a3-823a-a5b51992b031's
// primary proposal 1e3dd426-923d-46cf-8ee5-80b95a167036.
std::string fiveFilePredecessorJson()
{
    return R"({
        "schemaVersion": "proposal-submission.v2",
        "summary": "Add the navigation shell and its components",
        "changes": [
            {"change_id":"c1","operation":"create","relative_path":"qml/CMakeLists.txt","source_path":null,"proposed_content":"cmake-1"},
            {"change_id":"c2","operation":"create","relative_path":"qml/AppShell.qml","source_path":null,"proposed_content":"shell-1"},
            {"change_id":"c3","operation":"create","relative_path":"qml/components/navigation/AppSidebar.qml","source_path":null,"proposed_content":"sidebar-1"},
            {"change_id":"c4","operation":"create","relative_path":"qml/components/navigation/AppTopBar.qml","source_path":null,"proposed_content":"topbar-1"},
            {"change_id":"c5","operation":"create","relative_path":"qml/components/navigation/SidebarItem.qml","source_path":null,"proposed_content":"sidebaritem-1"}
        ]
    })";
}

// Materialization #1/#2/#3/#14: a one-file repair amendment over a five-file
// predecessor must produce one successor containing all five files - the
// amended file with repaired content, the other four byte-identical to the
// predecessor's own durable content - independently readable through only
// the successor's own changeSetId, and the live workspace must never be
// touched by any of it.
void testMaterializationCarriesForwardUntouchedPredecessorChanges()
{
    TempDir tmp;
    FakeChangeProposalRepository repo;
    auto storeResult = FileChangeProposalStore::open(tmp.path / "proposals");
    assert(storeResult.success());
    RunPathClaimRegistry registry;
    ChangeProposalService svc(repo, *storeResult.value(), &registry);

    auto predecessorRequest = makeServiceRequest(tmp.path / "project", fiveFilePredecessorJson());
    const auto predecessor = svc.createFromModelOutput(predecessorRequest);
    assert(predecessor.success());
    assert(predecessor.value().state == ChangeSetState::ReadyForReview);

    const std::string repairRaw = R"({
        "schemaVersion": "proposal-submission.v2",
        "summary": "Fix the AppTopBar.qml lint warning - falsely claims full replacement",
        "changes": [
            {"change_id":"repair-1","operation":"create","relative_path":"qml/components/navigation/AppTopBar.qml","source_path":null,"proposed_content":"topbar-2-fixed"}
        ]
    })";
    const auto successor = svc.materializeRepairSuccessor(
        makeMaterializeRequest(tmp.path / "project", predecessor.value().changeSetId, repairRaw));
    assert(successor.success());
    assert(successor.value().state == ChangeSetState::ReadyForReview);
    assert(successor.value().supersedesChangeSetId.has_value());
    assert(*successor.value().supersedesChangeSetId == predecessor.value().changeSetId);
    assert(successor.value().changeSetId != predecessor.value().changeSetId);

    const auto changesResult = svc.listChanges(successor.value().changeSetId);
    assert(changesResult.success());
    assert(changesResult.value().size() == 5);

    std::map<std::string, ChangeId> pathToChangeId;
    for (const auto& change : changesResult.value()) {
        pathToChangeId[change.relativePath.generic_string()] = change.changeId;
    }
    assert(pathToChangeId.count("qml/CMakeLists.txt") == 1);
    assert(pathToChangeId.count("qml/AppShell.qml") == 1);
    assert(pathToChangeId.count("qml/components/navigation/AppSidebar.qml") == 1);
    assert(pathToChangeId.count("qml/components/navigation/AppTopBar.qml") == 1);
    assert(pathToChangeId.count("qml/components/navigation/SidebarItem.qml") == 1);

    const auto topbarContent = svc.readProposedContent(
        successor.value().changeSetId, pathToChangeId.at("qml/components/navigation/AppTopBar.qml"));
    assert(topbarContent.success());
    assert(topbarContent.value() == "topbar-2-fixed");

    const std::map<std::string, std::string> untouched {
        {"qml/CMakeLists.txt", "cmake-1"},
        {"qml/AppShell.qml", "shell-1"},
        {"qml/components/navigation/AppSidebar.qml", "sidebar-1"},
        {"qml/components/navigation/SidebarItem.qml", "sidebaritem-1"},
    };
    for (const auto& [path, expectedContent] : untouched) {
        const auto got = svc.readProposedContent(successor.value().changeSetId, pathToChangeId.at(path));
        assert(got.success());
        assert(got.value() == expectedContent);
    }

    // #14: never written to the live workspace.
    assert(!fs::exists(tmp.path / "project" / "qml"));

    // Predecessor remains durable and unmutated.
    const auto reread = svc.findChangeSet(predecessor.value().changeSetId);
    assert(reread.success());
    assert(reread.value().state == ChangeSetState::ReadyForReview);
}

// Materialization #5: a genuinely new, non-conflicting repair change (not an
// amendment of any predecessor path) is appended, not dropped.
void testMaterializationIncludesGenuinelyNewNonConflictingChange()
{
    TempDir tmp;
    FakeChangeProposalRepository repo;
    auto storeResult = FileChangeProposalStore::open(tmp.path / "proposals");
    assert(storeResult.success());
    RunPathClaimRegistry registry;
    ChangeProposalService svc(repo, *storeResult.value(), &registry);

    const std::string predecessorRaw = R"({
        "schemaVersion": "proposal-submission.v2",
        "summary": "Add two files",
        "changes": [
            {"change_id":"c1","operation":"create","relative_path":"qml/CMakeLists.txt","source_path":null,"proposed_content":"cmake-1"},
            {"change_id":"c2","operation":"create","relative_path":"qml/AppShell.qml","source_path":null,"proposed_content":"shell-1"}
        ]
    })";
    const auto predecessor = svc.createFromModelOutput(
        makeServiceRequest(tmp.path / "project", predecessorRaw));
    assert(predecessor.success() && predecessor.value().state == ChangeSetState::ReadyForReview);

    const std::string repairRaw = R"({
        "schemaVersion": "proposal-submission.v2",
        "summary": "Add a genuinely new helper file discovered while repairing",
        "changes": [
            {"change_id":"repair-new","operation":"create","relative_path":"qml/NewHelper.qml","source_path":null,"proposed_content":"helper-1"}
        ]
    })";
    const auto successor = svc.materializeRepairSuccessor(
        makeMaterializeRequest(tmp.path / "project", predecessor.value().changeSetId, repairRaw));
    assert(successor.success());
    assert(successor.value().state == ChangeSetState::ReadyForReview);

    const auto changesResult = svc.listChanges(successor.value().changeSetId);
    assert(changesResult.success());
    assert(changesResult.value().size() == 3);
    bool foundNewHelper = false;
    for (const auto& change : changesResult.value()) {
        if (change.relativePath.generic_string() == "qml/NewHelper.qml") foundNewHelper = true;
    }
    assert(foundNewHelper);
}

// Materialization #6: two repair changes both amending the same predecessor
// footprint is ambiguous - fail closed with the dedicated stable code,
// never guess which one the model meant.
void testMaterializationRejectsAmbiguousRepairInternalOverlap()
{
    TempDir tmp;
    FakeChangeProposalRepository repo;
    auto storeResult = FileChangeProposalStore::open(tmp.path / "proposals");
    assert(storeResult.success());
    RunPathClaimRegistry registry;
    ChangeProposalService svc(repo, *storeResult.value(), &registry);

    const std::string predecessorRaw = R"({
        "schemaVersion": "proposal-submission.v2",
        "summary": "Add one file",
        "changes": [
            {"change_id":"c1","operation":"create","relative_path":"qml/AppShell.qml","source_path":null,"proposed_content":"shell-1"}
        ]
    })";
    const auto predecessor = svc.createFromModelOutput(
        makeServiceRequest(tmp.path / "project", predecessorRaw));
    assert(predecessor.success());

    const std::string repairRaw = R"({
        "schemaVersion": "proposal-submission.v2",
        "summary": "Two conflicting amendments of the same file",
        "changes": [
            {"change_id":"repair-a","operation":"create","relative_path":"qml/AppShell.qml","source_path":null,"proposed_content":"shell-2a"},
            {"change_id":"repair-b","operation":"create","relative_path":"qml/AppShell.qml","source_path":null,"proposed_content":"shell-2b"}
        ]
    })";
    const auto successor = svc.materializeRepairSuccessor(
        makeMaterializeRequest(tmp.path / "project", predecessor.value().changeSetId, repairRaw));
    assert(!successor.success());
    assert(successor.error()->code == "verification.repair_ambiguous_overlap");
}

// Materialization #7: a Rename whose destination collides with a
// carried-forward predecessor path is a final whole-set identity collision
// - fail closed, never silently let two changes claim the same target.
void testMaterializationRejectsRenameTargetCollisionWithCarriedPath()
{
    TempDir tmp;
    FakeChangeProposalRepository repo;
    auto storeResult = FileChangeProposalStore::open(tmp.path / "proposals");
    assert(storeResult.success());
    RunPathClaimRegistry registry;
    ChangeProposalService svc(repo, *storeResult.value(), &registry);

    const std::string predecessorRaw = R"({
        "schemaVersion": "proposal-submission.v2",
        "summary": "Add two files",
        "changes": [
            {"change_id":"c1","operation":"create","relative_path":"qml/a.qml","source_path":null,"proposed_content":"a-1"},
            {"change_id":"c2","operation":"create","relative_path":"qml/b.qml","source_path":null,"proposed_content":"b-1"}
        ]
    })";
    const auto predecessor = svc.createFromModelOutput(
        makeServiceRequest(tmp.path / "project", predecessorRaw));
    assert(predecessor.success());

    // Renames a.qml (the repair's own footprint = sourcePath = "qml/a.qml",
    // so this replaces predecessor c1) to "qml/b.qml" - which the
    // carried-forward c2 ("qml/b.qml") already occupies.
    const std::string repairRaw = R"({
        "schemaVersion": "proposal-submission.v2",
        "summary": "Rename a.qml to the already-taken b.qml",
        "changes": [
            {"change_id":"repair-rename","operation":"rename","relative_path":"qml/b.qml","source_path":"qml/a.qml","proposed_content":null}
        ]
    })";
    const auto successor = svc.materializeRepairSuccessor(
        makeMaterializeRequest(tmp.path / "project", predecessor.value().changeSetId, repairRaw,
            /*allowDelete=*/false, /*allowRename=*/true));
    assert(!successor.success());
    assert(successor.error()->code == "verification.repair_ambiguous_overlap");
}

// Materialization #8: a missing predecessor fails closed before touching
// anything else.
void testMaterializationRejectsMissingPredecessor()
{
    TempDir tmp;
    FakeChangeProposalRepository repo;
    auto storeResult = FileChangeProposalStore::open(tmp.path / "proposals");
    assert(storeResult.success());
    ChangeProposalService svc(repo, *storeResult.value());

    const std::string repairRaw = R"({
        "schemaVersion": "proposal-submission.v2",
        "summary": "Amend a predecessor that does not exist",
        "changes": [
            {"change_id":"repair-1","operation":"create","relative_path":"qml/AppShell.qml","source_path":null,"proposed_content":"shell-2"}
        ]
    })";
    const auto successor = svc.materializeRepairSuccessor(
        makeMaterializeRequest(tmp.path / "project", "nonexistent-changeset-id", repairRaw));
    assert(!successor.success());
    assert(successor.error()->code == "verification.repair_predecessor_missing");
}

// Materialization #9: a predecessor belonging to a different Run/project
// fails closed - the trusted runId/projectId are cross-checked, never
// trusted in isolation.
void testMaterializationRejectsMismatchedRunOrProject()
{
    TempDir tmp;
    FakeChangeProposalRepository repo;
    auto storeResult = FileChangeProposalStore::open(tmp.path / "proposals");
    assert(storeResult.success());
    RunPathClaimRegistry registry;
    ChangeProposalService svc(repo, *storeResult.value(), &registry);

    const std::string predecessorRaw = R"({
        "schemaVersion": "proposal-submission.v2",
        "summary": "Add one file",
        "changes": [
            {"change_id":"c1","operation":"create","relative_path":"qml/AppShell.qml","source_path":null,"proposed_content":"shell-1"}
        ]
    })";
    const auto predecessor = svc.createFromModelOutput(
        makeServiceRequest(tmp.path / "project", predecessorRaw));
    assert(predecessor.success());

    const std::string repairRaw = R"({
        "schemaVersion": "proposal-submission.v2",
        "summary": "Amend from a different Run",
        "changes": [
            {"change_id":"repair-1","operation":"create","relative_path":"qml/AppShell.qml","source_path":null,"proposed_content":"shell-2"}
        ]
    })";
    auto materializeRequest = makeMaterializeRequest(
        tmp.path / "project", predecessor.value().changeSetId, repairRaw);
    materializeRequest.runId = "a-completely-different-run";
    const auto successor = svc.materializeRepairSuccessor(materializeRequest);
    assert(!successor.success());
    assert(successor.error()->code == "verification.repair_predecessor_mismatch");
}

// Materialization #10: a genuinely new repair change outside the frozen
// task's own write policy is rejected by the ordinary downstream validator
// re-run, exactly like any other out-of-scope proposal - the materialized
// successor is durably persisted as Rejected, not silently dropped or
// silently allowed.
void testMaterializationRejectsOutOfScopeNewChange()
{
    TempDir tmp;
    FakeChangeProposalRepository repo;
    auto storeResult = FileChangeProposalStore::open(tmp.path / "proposals");
    assert(storeResult.success());
    RunPathClaimRegistry registry;
    ChangeProposalService svc(repo, *storeResult.value(), &registry);

    const std::string predecessorRaw = R"({
        "schemaVersion": "proposal-submission.v2",
        "summary": "Add one file inside qml/",
        "changes": [
            {"change_id":"c1","operation":"create","relative_path":"qml/AppShell.qml","source_path":null,"proposed_content":"shell-1"}
        ]
    })";
    auto predecessorRequest = makeServiceRequest(tmp.path / "project", predecessorRaw);
    predecessorRequest.policy.allowedRoots = {"qml"};
    const auto predecessor = svc.createFromModelOutput(predecessorRequest);
    assert(predecessor.success() && predecessor.value().state == ChangeSetState::ReadyForReview);

    const std::string repairRaw = R"({
        "schemaVersion": "proposal-submission.v2",
        "summary": "Add a file outside the frozen task's own scope",
        "changes": [
            {"change_id":"repair-1","operation":"create","relative_path":"src/forbidden.cpp","source_path":null,"proposed_content":"// out of scope"}
        ]
    })";
    auto materializeRequest = makeMaterializeRequest(
        tmp.path / "project", predecessor.value().changeSetId, repairRaw);
    materializeRequest.policy.allowedRoots = {"qml"};
    const auto successor = svc.materializeRepairSuccessor(materializeRequest);
    assert(successor.success());
    assert(successor.value().state == ChangeSetState::Rejected);
    bool foundOutOfScope = false;
    for (const auto& finding : successor.value().validationFindings) {
        if (finding.code == "proposal.outside_allowed_roots") foundOutOfScope = true;
    }
    assert(foundOutOfScope);
}

// Materialization #11: a carried-forward Modify change (never touched by
// the repair) keeps a durable, trusted hash precondition - reused from the
// predecessor's own already-Backend-observed evidence, never a model-typed
// value, and sufficient on its own to pass the ordinary
// proposal.missing_hash_evidence gate a second time.
void testMaterializationRetainsTrustedHashEvidenceForCarriedModify()
{
    TempDir tmp;
    FakeChangeProposalRepository repo;
    auto storeResult = FileChangeProposalStore::open(tmp.path / "proposals");
    assert(storeResult.success());
    RunPathClaimRegistry registry;
    ChangeProposalService svc(repo, *storeResult.value(), &registry);

    const fs::path projectRoot = tmp.path / "project";
    fs::create_directories(projectRoot / "qml");
    const std::string originalModifyContent = "original-modify-content";
    {
        std::ofstream out(projectRoot / "qml" / "Existing.qml", std::ios::binary | std::ios::trunc);
        out << originalModifyContent;
    }
    const std::string observedHash = sha256Hex(originalModifyContent);

    const std::string predecessorRaw = R"({
        "schemaVersion": "proposal-submission.v2",
        "summary": "Modify an existing file and add a new one",
        "changes": [
            {"change_id":"c1","operation":"modify","relative_path":"qml/Existing.qml","source_path":null,"proposed_content":"modified-content"},
            {"change_id":"c2","operation":"create","relative_path":"qml/AppShell.qml","source_path":null,"proposed_content":"shell-1"}
        ]
    })";
    auto predecessorRequest = makeServiceRequest(projectRoot, predecessorRaw);
    predecessorRequest.requireHashEvidence = true;
    predecessorRequest.observedFileHashes["qml/existing.qml"] = observedHash;
    const auto predecessor = svc.createFromModelOutput(predecessorRequest);
    assert(predecessor.success());
    assert(predecessor.value().state == ChangeSetState::ReadyForReview);

    // The repair amends ONLY AppShell.qml - it never touches Existing.qml,
    // and its own observedFileHashes (a fresh audited-session map, exactly
    // like a real repair worker's) carries no entry for it either.
    const std::string repairRaw = R"({
        "schemaVersion": "proposal-submission.v2",
        "summary": "Fix AppShell.qml only",
        "changes": [
            {"change_id":"repair-1","operation":"create","relative_path":"qml/AppShell.qml","source_path":null,"proposed_content":"shell-2-fixed"}
        ]
    })";
    const auto successor = svc.materializeRepairSuccessor(
        makeMaterializeRequest(projectRoot, predecessor.value().changeSetId, repairRaw));
    assert(successor.success());
    assert(successor.value().state == ChangeSetState::ReadyForReview);

    const auto changesResult = svc.listChanges(successor.value().changeSetId);
    assert(changesResult.success());
    bool foundCarriedModify = false;
    for (const auto& change : changesResult.value()) {
        if (change.relativePath.generic_string() == "qml/Existing.qml") {
            foundCarriedModify = true;
            assert(change.operation == FileChangeOperation::Modify);
            assert(change.expectedOriginalHash.has_value());
            assert(*change.expectedOriginalHash == observedHash);
        }
    }
    assert(foundCarriedModify);

    // #14: the live workspace's file is byte-identical - never modified.
    std::ifstream check(projectRoot / "qml" / "Existing.qml", std::ios::binary);
    std::string onDisk((std::istreambuf_iterator<char>(check)), std::istreambuf_iterator<char>());
    assert(onDisk == originalModifyContent);
}

// Materialization #12: the wire contract never accepts a model-authored
// hash, in a repair amendment exactly as in any other submission - the
// same ProposedChangeSetParser field-set rejection applies unchanged.
void testMaterializationRejectsModelAuthoredHashInAmendment()
{
    TempDir tmp;
    FakeChangeProposalRepository repo;
    auto storeResult = FileChangeProposalStore::open(tmp.path / "proposals");
    assert(storeResult.success());
    RunPathClaimRegistry registry;
    ChangeProposalService svc(repo, *storeResult.value(), &registry);

    const std::string predecessorRaw = R"({
        "schemaVersion": "proposal-submission.v2",
        "summary": "Add one file",
        "changes": [
            {"change_id":"c1","operation":"create","relative_path":"qml/AppShell.qml","source_path":null,"proposed_content":"shell-1"}
        ]
    })";
    const auto predecessor = svc.createFromModelOutput(
        makeServiceRequest(tmp.path / "project", predecessorRaw));
    assert(predecessor.success());

    const std::string repairRawWithHash = R"({
        "schemaVersion": "proposal-submission.v2",
        "summary": "Attempt to smuggle a hash",
        "changes": [
            {"change_id":"repair-1","operation":"create","relative_path":"qml/AppShell.qml","source_path":null,"proposed_content":"shell-2","expected_original_hash":"deadbeef"}
        ]
    })";
    const auto successor = svc.materializeRepairSuccessor(makeMaterializeRequest(
        tmp.path / "project", predecessor.value().changeSetId, repairRawWithHash));
    assert(!successor.success());
    assert(successor.error()->code == "proposal.unexpected_field");
}

// Minimal IChangeProposalRepository decorator that can be told to fail the
// NEXT insertChangeSet() call - used only to exercise materialization
// #13's downstream-persistence-failure path deterministically. Forwards
// every other call unchanged to the wrapped FakeChangeProposalRepository.
class FailNextInsertChangeProposalRepository final : public IChangeProposalRepository {
public:
    explicit FailNextInsertChangeProposalRepository(IChangeProposalRepository& inner)
        : inner_(inner)
    {
    }
    bool failNextInsert = false;

    common::Result<void> insertChangeSet(const ProposedChangeSet& cs) override
    {
        if (failNextInsert) {
            failNextInsert = false;
            return common::Result<void>::failure(
                {"proposal.simulated_storage_failure", "Injected failure for a materialization test"});
        }
        return inner_.insertChangeSet(cs);
    }
    common::Result<ProposedChangeSet> updateChangeSetState(
        const ChangeSetId& id, ChangeSetState newState) override
    {
        return inner_.updateChangeSetState(id, newState);
    }
    common::Result<void> markMissingEvidence(
        const ChangeSetId& id, std::string code, std::string message) override
    {
        return inner_.markMissingEvidence(id, std::move(code), std::move(message));
    }
    common::Result<std::optional<ProposedChangeSet>> findChangeSet(
        const ChangeSetId& id) const override
    {
        return inner_.findChangeSet(id);
    }
    common::Result<std::vector<ProposedChangeSet>> listForRun(const RunId& runId) const override
    {
        return inner_.listForRun(runId);
    }
    common::Result<std::vector<ProposedChangeSet>> listForProject(
        const ProjectId& projectId) const override
    {
        return inner_.listForProject(projectId);
    }
    common::Result<std::vector<ProposedFileChange>> listChanges(
        const ChangeSetId& changeSetId) const override
    {
        return inner_.listChanges(changeSetId);
    }
    common::Result<std::vector<ValidationFinding>> listValidationFindings(
        const ChangeSetId& changeSetId) const override
    {
        return inner_.listValidationFindings(changeSetId);
    }
    common::Result<std::unordered_map<ChangeSetId, std::vector<ProposedFileChange>>>
    listChangesForSets(const std::vector<ChangeSetId>& changeSetIds) const override
    {
        return inner_.listChangesForSets(changeSetIds);
    }
    common::Result<std::unordered_map<ChangeSetId, std::vector<ValidationFinding>>>
    listValidationFindingsForSets(const std::vector<ChangeSetId>& changeSetIds) const override
    {
        return inner_.listValidationFindingsForSets(changeSetIds);
    }

private:
    IChangeProposalRepository& inner_;
};

// Materialization #13: a repository-insert failure AFTER the successor
// already atomically acquired the predecessor's claim must restore the
// predecessor's exact original claim, never leave it lost.
void testMaterializationRestoresPredecessorClaimOnDownstreamFailure()
{
    TempDir tmp;
    FakeChangeProposalRepository innerRepo;
    FailNextInsertChangeProposalRepository repo(innerRepo);
    auto storeResult = FileChangeProposalStore::open(tmp.path / "proposals");
    assert(storeResult.success());
    RunPathClaimRegistry registry;
    ChangeProposalService svc(repo, *storeResult.value(), &registry);

    const std::string predecessorRaw = R"({
        "schemaVersion": "proposal-submission.v2",
        "summary": "Add one file",
        "changes": [
            {"change_id":"c1","operation":"create","relative_path":"qml/AppShell.qml","source_path":null,"proposed_content":"shell-1"}
        ]
    })";
    const auto predecessor = svc.createFromModelOutput(
        makeServiceRequest(tmp.path / "project", predecessorRaw));
    assert(predecessor.success());
    assert(predecessor.value().state == ChangeSetState::ReadyForReview);

    repo.failNextInsert = true;
    const std::string repairRaw = R"({
        "schemaVersion": "proposal-submission.v2",
        "summary": "A valid amendment that will fail to persist",
        "changes": [
            {"change_id":"repair-1","operation":"create","relative_path":"qml/AppShell.qml","source_path":null,"proposed_content":"shell-2"}
        ]
    })";
    const auto successor = svc.materializeRepairSuccessor(
        makeMaterializeRequest(tmp.path / "project", predecessor.value().changeSetId, repairRaw));
    assert(!successor.success());
    assert(successor.error()->code == "proposal.simulated_storage_failure");

    // The predecessor's claim must have been restored: a third, unrelated
    // changeset attempting to claim the exact same path must still find it
    // held (by the predecessor), proving the failed transfer did not leave
    // the path free or orphaned.
    const auto thirdPartyAttempt = registry.tryClaim(
        "run-svc-001", "some-unrelated-changeset",
        {RunPathClaimRegistry::PathClaim {"qml/AppShell.qml", "unrelated-change"}});
    assert(!thirdPartyAttempt.claimed);
    assert(thirdPartyAttempt.conflictingChangeSetId == predecessor.value().changeSetId);

    // The predecessor row itself remains durable and unmutated.
    const auto reread = svc.findChangeSet(predecessor.value().changeSetId);
    assert(reread.success());
    assert(reread.value().state == ChangeSetState::ReadyForReview);
}

// Hardening follow-up #2: when the predecessor never actually held an
// in-memory path claim (e.g. it was created through a caller with no
// RunPathClaimRegistry wired) and a later materialization's downstream
// persistence still fails after a successful forward transferClaim, the
// rollback must NOT fabricate a claim for the predecessor that it never
// actually had - it must reuse transferClaim's own exact previousClaims
// snapshot (empty here), never a separately reconstructed path list built
// from the predecessor's structural changes.
void testMaterializationRollbackNeverFabricatesPhantomPredecessorClaim()
{
    TempDir tmp;
    FakeChangeProposalRepository innerRepo;
    FailNextInsertChangeProposalRepository repo(innerRepo);
    auto storeResult = FileChangeProposalStore::open(tmp.path / "proposals");
    assert(storeResult.success());
    RunPathClaimRegistry registry;

    // The predecessor is created through a service instance with NO
    // path-claim registry wired - it reaches ReadyForReview but never
    // actually claims anything in `registry`.
    ChangeProposalService svcNoRegistry(repo, *storeResult.value());
    const std::string predecessorRaw = R"({
        "schemaVersion": "proposal-submission.v2",
        "summary": "Add one file",
        "changes": [
            {"change_id":"c1","operation":"create","relative_path":"qml/AppShell.qml","source_path":null,"proposed_content":"shell-1"}
        ]
    })";
    const auto predecessor = svcNoRegistry.createFromModelOutput(
        makeServiceRequest(tmp.path / "project", predecessorRaw));
    assert(predecessor.success());
    assert(predecessor.value().state == ChangeSetState::ReadyForReview);

    // Confirm the predecessor genuinely holds no claim in `registry` yet.
    const auto preCheck = registry.tryClaim(
        "run-svc-001", "pre-check-changeset",
        {RunPathClaimRegistry::PathClaim {"qml/AppShell.qml", "unrelated"}});
    assert(preCheck.claimed);
    registry.release("run-svc-001", "pre-check-changeset");

    ChangeProposalService svcWithRegistry(repo, *storeResult.value(), &registry);
    repo.failNextInsert = true;
    const std::string repairRaw = R"({
        "schemaVersion": "proposal-submission.v2",
        "summary": "A valid amendment that will fail to persist",
        "changes": [
            {"change_id":"repair-1","operation":"create","relative_path":"qml/AppShell.qml","source_path":null,"proposed_content":"shell-2"}
        ]
    })";
    const auto successor = svcWithRegistry.materializeRepairSuccessor(
        makeMaterializeRequest(tmp.path / "project", predecessor.value().changeSetId, repairRaw));
    assert(!successor.success());
    assert(successor.error()->code == "proposal.simulated_storage_failure");

    // No phantom claim must have been fabricated for the predecessor: an
    // unrelated changeset must still be able to freely claim the same path.
    const auto postCheck = registry.tryClaim(
        "run-svc-001", "post-check-changeset",
        {RunPathClaimRegistry::PathClaim {"qml/AppShell.qml", "unrelated-2"}});
    assert(postCheck.claimed);
}

} // namespace

int runChangeProposalTests()
{
    // Parser
    testParseValidCreateRawJson();
    testParseNonAsciiRelativePathPreservesUtf8();
    testParseValidCreateFenced();
    testParseValidModify();
    testParseValidDelete();
    testParseValidRename();
    testParseEmptyChangesArray();
    testParseMalformedJson();
    testParseExtraContentBeforeFence();
    testParseExtraContentAfterFence();
    testParseMultipleFences();
    testParseFenceContentContainingEmbeddedBacktickMention();
    testParseMissingSummary();
    testParseMissingChangesArray();
    testParseMissingSchemaVersion();
    testParseWrongSchemaVersionIsRejected();
    testParseUnknownOperation();
    testParseEmptyChangeId();
    testParseUnexpectedTopLevelFieldIsRejected();
    testParseTrustedMetadataAlwaysWins();
    testParseRejectsOldHashFieldAsUnexpected();
    testParseRejectsWrongOptionalFieldType();
    testParseCreateWithEmptySourcePathIsRejected();
    testParseCreateWithEmptyProposedContentIsPreserved();
    testParseDeleteWithEmptyProposedContentIsRejected();
    testParseRenameWithEmptyProposedContentIsRejected();
    testParserEnforcesAdvertisedStringBounds();

    // Validator
    testValidatePathTraversal();
    testValidateAbsolutePath();
    testValidateGitPath();
    testValidateSecretFilename();
    testValidateBuildDir();
    testValidateAllowedRootsViolation();
    testValidateDeniedPath();
    testValidateExtensionNotAllowed();
    testValidateMaxFilesExceeded();
    testValidateMaxBytesExceeded();
    testValidateDeleteWithoutPermission();
    testValidateRenameWithoutPermission();
    testValidateDuplicateChangeId();
    testValidateConflictingPaths();
    testValidateBinaryContent();
    testValidateCreateWithoutContent();
    testValidateModifyWithoutHash();
    testValidateModifyWithoutContent();
    testValidateHashMismatch();
    testValidateHashMatch();
    testValidateHashEvidenceMissingIsRejected();
    testValidateHashEvidenceOverridesWrongModelString();
    testValidateHashEvidenceStaleFileStillRejected();
    testValidateCleanProposalNoFindings();
    testValidateNestedAllowedRoot();
    testValidateExactWriteAllocationDoesNotAuthorizeDescendants();
    testValidateRenameSourcePolicy();
#ifdef _WIN32
    testValidateWindowsCaseInsensitiveProtectedPaths();
#endif

    // Secret scanner
    testSecretScanAwsKeyInContent();
    testSecretScanOpenAiKeyInSummary();
    testSecretScanPemKeyInContent();
    testSecretScanCleanContentPasses();
    testSecretScanNeverEchosValue();

    // Store
    testStorePublishAndReadContent();
    testStoreDuplicateChangeSetIdRejected();
    testStoreUnsafeIdRejected();
    testStoreSupportsPathsLongerThanMaxPath();

    // Service integration
    testServiceCreateReadyForReview();
    testServiceCreateTargetExistsIsRejected();
    testServiceCreateNoChangesNeeded();
    testCrossAgentConflictRejectsSecondProposalSamePath();
    testCrossAgentConflictDoesNotBlockDifferentPaths();
    testNoRegistryMeansNoCrossAgentCheck();
    testServiceSecretDetectedNothingStored();
    testServiceMalformedJsonNothingStored();
    testServiceListAndFindQueries();
    testServiceReadContent();
    testServiceUnsafePathNeverSnapshotsOutsideRoot();
    testServiceSecretInOriginalSnapshotIsNotStored();

    // SQLite round-trip
    testSqliteChangeProposalRoundTrip();
    testSqliteRenameSourcePathPreservesUtf8();
    testSqliteSchemaV20Migration();
    testChangeSetPersistsModelExecutionIdMissingEvidenceAndSupersession();

    // Batched reads (listChangesForSets / listValidationFindingsForSets)
    testBatchedReadsEmptyInputReturnsEmptySuccess();
    testBatchedReadsSingleChangeSet();
    testBatchedReadsMultipleChangeSetsDoNotCrossContaminate();
    testBatchedReadsMissingIdIsNotAnError();
    testBatchedReadsDuplicateIdsProduceNoDuplicateResults();
    testBatchedReadsPreserveOrdinalOrderWithinChangeSet();
    testBatchedReadsChunkLargeIdListAcrossParameterLimit();
    testBatchedReadsSurfaceStatementFailureWithoutPartialSuccess();

    // Approval audit trail (schema v25)
    testSqliteChangeApprovalRoundTrip();
    testSqliteSchemaV25Migration();
    testSqliteSchemaV36AddsPostApplyFingerprintColumn();

    // Proposal review / reject-discard (AG-07, schema v30)
    testProposalReviewRejectSucceedsFromReadyForReview();
    testProposalReviewRejectFailsWhenNotReadyForReview();
    testProposalReviewRejectFailsForUnknownChangeSet();
    testProposalReviewRejectIsIdempotent();
    testProposalReviewRejectNudgesTheOutcomeBriefOnce();
    testProposalReviewRejectReasonTooLarge();
    testProposalReviewAssertNotRejectedPassesWhenClean();
    testProposalReviewAssertNotRejectedFailsAfterRejection();
    testSqliteProposalReviewRoundTrip();
    testSqliteProposalReviewRejectionWithoutReasonRoundTrips();
    testSqliteSchemaV30Migration();

    // Lineage-aware verification-repair materialization
    testMaterializationCarriesForwardUntouchedPredecessorChanges();
    testMaterializationIncludesGenuinelyNewNonConflictingChange();
    testMaterializationRejectsAmbiguousRepairInternalOverlap();
    testMaterializationRejectsRenameTargetCollisionWithCarriedPath();
    testMaterializationRejectsMissingPredecessor();
    testMaterializationRejectsMismatchedRunOrProject();
    testMaterializationRejectsOutOfScopeNewChange();
    testMaterializationRetainsTrustedHashEvidenceForCarriedModify();
    testMaterializationRejectsModelAuthoredHashInAmendment();
    testMaterializationRestoresPredecessorClaimOnDownstreamFailure();
    testMaterializationRollbackNeverFabricatesPhantomPredecessorClaim();

    return 0;
}
