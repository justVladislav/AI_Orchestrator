#include "ai/infrastructure/FileChangeProposalStore.hpp"

#include "ai/application/artifacts/ArtifactPathPolicy.hpp"
#include "ai/application/workspace/FilesystemPathUtils.hpp"
#include "ai/application/IdGenerator.hpp"
#include "ai/application/workspace/ProjectWritePolicy.hpp"
#include "ai/application/Sha256.hpp"

#include <nlohmann/json.hpp>

#include <fstream>
#include <sstream>
#include <system_error>
#include <unordered_set>

namespace ai::infrastructure {

namespace {

namespace fs = std::filesystem;
using json = nlohmann::json;

common::Error storeError(std::string code, std::string msg)
{
    return {std::move(code), std::move(msg)};
}

// Every filesystem call of this store (std::filesystem AND stream opens) goes through longPath(): the files of a proposal sit 164
// characters below the store root (full SHA-256 names, kept). See ai::application::detail::longPath for the why.
using ai::application::detail::longPath;
using ai::application::detail::withoutLongPrefix;

fs::path streamPath(const fs::path& path)
{
    return longPath(path);
}

bool isContainedIn(const fs::path& root, const fs::path& candidate)
{
    // Both sides in the same spelling; the checks themselves are unchanged: every component of the root must match.
    const fs::path plainRoot = withoutLongPrefix(root);
    const fs::path plainCandidate = withoutLongPrefix(candidate);
    auto rootIt = plainRoot.begin();
    auto candIt = plainCandidate.begin();
    for (; rootIt != plainRoot.end(); ++rootIt, ++candIt) {
        if (candIt == plainCandidate.end() || *candIt != *rootIt) return false;
    }
    return true;
}

common::Result<void> writeFile(const fs::path& path, std::string_view content)
{
    std::ofstream out(streamPath(path), std::ios::binary | std::ios::trunc);
    if (!out) {
        return common::Result<void>::failure(
            storeError("proposal_store.write_failed",
                       "Could not open file for writing: " + application::detail::genericUtf8(path.filename())));
    }
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
    if (!out) {
        return common::Result<void>::failure(
            storeError("proposal_store.write_failed",
                       "Could not write to file: " + application::detail::genericUtf8(path.filename())));
    }
    return common::Result<void>::ok();
}

common::Result<std::string> readFile(const fs::path& path)
{
    std::ifstream in(streamPath(path), std::ios::binary);
    if (!in) {
        return common::Result<std::string>::failure(
            storeError("proposal_store.read_failed",
                       "Could not open file: " + application::detail::genericUtf8(path.filename())));
    }
    std::ostringstream buf;
    buf << in.rdbuf();
    if (!in) {
        return common::Result<std::string>::failure(
            storeError("proposal_store.read_failed",
                       "Could not read file: " + application::detail::genericUtf8(path.filename())));
    }
    return common::Result<std::string>::ok(buf.str());
}

// Build a policy JSON snapshot string.
std::string buildPolicyJson(const application::ProjectWritePolicy& policy)
{
    json j;
    // Persist an identity hash, not the user's absolute local path.
    j["project_root_hash"]      =
        application::sha256Hex(application::detail::genericUtf8(policy.projectRoot));
    j["allow_delete"]           = policy.allowDelete;
    j["allow_rename"]           = policy.allowRename;
    j["max_files_per_change_set"] = policy.maxFilesPerChangeSet;
    j["max_total_bytes"]        = policy.maxTotalBytes;

    json allowedRoots = json::array();
    for (const auto& r : policy.allowedRoots) allowedRoots.push_back(application::detail::genericUtf8(r));
    j["allowed_roots"] = allowedRoots;

    json deniedPaths = json::array();
    for (const auto& d : policy.deniedPaths) deniedPaths.push_back(application::detail::genericUtf8(d));
    j["denied_paths"] = deniedPaths;

    json allowedExts = json::array();
    for (const auto& e : policy.allowedExtensions) allowedExts.push_back(e);
    j["allowed_extensions"] = allowedExts;

    return j.dump(2);
}

// Build the change-set manifest (no content blobs, only metadata).
std::string buildManifestJson(const application::ProposedChangeSet& cs)
{
    json j;
    j["schema_version"]        = cs.schemaVersion;
    j["change_set_id"]         = cs.changeSetId;
    j["run_id"]                = cs.runId;
    j["project_id"]            = cs.projectId;
    j["provider_id"]           = cs.providerId;
    j["model_id"]              = cs.modelId;
    if (cs.modelExecutionId.has_value())
        j["model_execution_id"] = *cs.modelExecutionId;
    j["agent_role"]            = cs.agentRole;
    j["summary"]               = cs.summary;
    j["base_revision"]         = cs.baseRevision;
    j["state"]                 = application::toString(cs.state);
    j["created_at_ms"]         = std::chrono::duration_cast<std::chrono::milliseconds>(
                                      cs.createdAt.time_since_epoch()).count();

    json changesArr = json::array();
    for (const auto& ch : cs.changes) {
        json c;
        c["change_id"]              = ch.changeId;
        c["operation"]              = application::toString(ch.operation);
        c["relative_path"]          = application::detail::genericUtf8(ch.relativePath);
        if (ch.sourcePath.has_value())
            c["source_path"] = application::detail::genericUtf8(*ch.sourcePath);
        if (ch.expectedOriginalHash.has_value())
            c["expected_original_hash"] = *ch.expectedOriginalHash;
        changesArr.push_back(std::move(c));
    }
    j["changes"] = std::move(changesArr);
    return j.dump(2);
}

std::string buildValidationJson(const std::vector<application::ValidationFinding>& findings)
{
    json j = json::array();
    for (const auto& f : findings) {
        json e;
        e["code"]      = f.code;
        e["message"]   = f.message;
        e["change_id"] = f.changeId;
        j.push_back(std::move(e));
    }
    return j.dump(2);
}

// Validate that changeSetId and runId cannot be used for path traversal.
// They must be non-empty and consist only of safe characters (UUID-style).
bool isSafeSegment(const std::string& s)
{
    if (s.empty()) return false;
    for (char c : s) {
        if (!std::isalnum(static_cast<unsigned char>(c))
            && c != '-' && c != '_') {
            return false;
        }
    }
    return true;
}

} // namespace

FileChangeProposalStore::FileChangeProposalStore(std::filesystem::path canonicalRoot)
    : root_(std::move(canonicalRoot))
{
}

common::Result<std::unique_ptr<FileChangeProposalStore>> FileChangeProposalStore::open(
    const std::filesystem::path& root)
{
    if (root.empty()) {
        return common::Result<std::unique_ptr<FileChangeProposalStore>>::failure(
            storeError("proposal_store.empty_root", "Proposal store root cannot be empty"));
    }

    std::error_code ec;
    fs::create_directories(longPath(root), ec);
    if (ec) {
        return common::Result<std::unique_ptr<FileChangeProposalStore>>::failure(
            storeError("proposal_store.directory_failed",
                       "Could not create proposal store root: " + ec.message()));
    }

    // The root is kept in its ordinary spelling (no "\\?\"); every filesystem call converts it with longPath().
    fs::path canonical = withoutLongPrefix(fs::canonical(longPath(root), ec));
    if (ec) {
        return common::Result<std::unique_ptr<FileChangeProposalStore>>::failure(
            storeError("proposal_store.canonicalize_failed",
                       "Could not canonicalize proposal store root: " + ec.message()));
    }

    return common::Result<std::unique_ptr<FileChangeProposalStore>>::ok(
        std::unique_ptr<FileChangeProposalStore>(
            new FileChangeProposalStore(std::move(canonical))));
}

common::Result<application::PublishedProposalLocation> FileChangeProposalStore::publishChangeSet(
    const application::ProposedChangeSet& changeSet,
    const application::ProjectWritePolicy& policy)
{
    std::lock_guard lock(mutex_);

    // Security: changeSetId and runId must not be usable for path traversal.
    if (!isSafeSegment(changeSet.changeSetId)) {
        return common::Result<application::PublishedProposalLocation>::failure(
            storeError("proposal_store.unsafe_id", "changeSetId contains unsafe characters"));
    }
    if (!isSafeSegment(changeSet.runId)) {
        return common::Result<application::PublishedProposalLocation>::failure(
            storeError("proposal_store.unsafe_id", "runId contains unsafe characters"));
    }

    // Build paths.
    const std::string sanitizedRun = application::sanitizePathSegment(changeSet.runId);
    const std::string sanitizedCs  = application::sanitizePathSegment(changeSet.changeSetId);

    const std::string relRoot = "runs/" + sanitizedRun + "/proposals/" + sanitizedCs;
    const fs::path finalDir   = root_ / fs::path(relRoot);
    const fs::path stagingDir = root_ / "runs" / sanitizedRun / "proposals"
                              / (".staging_" + sanitizedCs + "_"
                                  + application::generateUuid());

    // Reject duplicate (final dir already exists).
    {
        std::error_code ec;
        if (fs::exists(longPath(finalDir), ec)) {
            return common::Result<application::PublishedProposalLocation>::failure(
                storeError("proposal_store.duplicate_id",
                           "A proposal with this changeSetId already exists"));
        }
    }

    // --- Write to staging ---------------------------------------------------
    {
        std::error_code ec;
        fs::create_directories(longPath(stagingDir), ec);
        if (ec) {
            return common::Result<application::PublishedProposalLocation>::failure(
                storeError("proposal_store.directory_failed",
                           "Could not create staging directory: " + ec.message()));
        }
    }

    // Helper: on any error, clean up the staging directory.
    auto cleanup = [&] {
        std::error_code ec;
        fs::remove_all(longPath(stagingDir), ec);
    };

    // 1. manifest.json
    {
        auto r = writeFile(stagingDir / "manifest.json", buildManifestJson(changeSet));
        if (!r.success()) { cleanup(); return common::Result<application::PublishedProposalLocation>::failure(*r.error()); }
    }

    // 2. summary.md
    {
        auto r = writeFile(stagingDir / "summary.md", changeSet.summary);
        if (!r.success()) { cleanup(); return common::Result<application::PublishedProposalLocation>::failure(*r.error()); }
    }

    // 3. policy.json
    const std::string policyJson = buildPolicyJson(policy);
    {
        auto r = writeFile(stagingDir / "policy.json", policyJson);
        if (!r.success()) { cleanup(); return common::Result<application::PublishedProposalLocation>::failure(*r.error()); }
    }
    const std::string policyHash = application::sha256Hex(policyJson);

    // 4. validation.json
    {
        auto r = writeFile(stagingDir / "validation.json",
                           buildValidationJson(changeSet.validationFindings));
        if (!r.success()) { cleanup(); return common::Result<application::PublishedProposalLocation>::failure(*r.error()); }
    }

    // 5. originals/ and proposed/ directories + content files
    {
        std::error_code ec;
        fs::create_directories(longPath(stagingDir / "originals"), ec);
        fs::create_directories(longPath(stagingDir / "proposed"), ec);
        if (ec) {
            cleanup();
            return common::Result<application::PublishedProposalLocation>::failure(
                storeError("proposal_store.directory_failed",
                           "Could not create content directories: " + ec.message()));
        }
    }

    std::unordered_set<std::string> seenChangeIds;
    for (const auto& ch : changeSet.changes) {
        if (ch.changeId.empty() || !seenChangeIds.insert(ch.changeId).second) {
            cleanup();
            return common::Result<application::PublishedProposalLocation>::failure(
                storeError(
                    "proposal_store.invalid_change_id",
                    "Change identifiers must be non-empty and unique"));
        }
        // Model-controlled identifiers never become path segments directly.
        const std::string safeId = application::sha256Hex(ch.changeId);

        // Original snapshot (Modify / Delete / Rename)
        if (ch.capturedOriginalContent.has_value()) {
            const fs::path orig = stagingDir / "originals" / safeId;
            auto r = writeFile(orig, *ch.capturedOriginalContent);
            if (!r.success()) { cleanup(); return common::Result<application::PublishedProposalLocation>::failure(*r.error()); }
        }

        // Proposed content (Create / Modify)
        if (ch.proposedContent.has_value()) {
            const fs::path prop = stagingDir / "proposed" / safeId;
            auto r = writeFile(prop, *ch.proposedContent);
            if (!r.success()) { cleanup(); return common::Result<application::PublishedProposalLocation>::failure(*r.error()); }
        }
    }

    // --- Atomic rename staging → final -------------------------------------
    // Ensure parent exists.
    {
        std::error_code ec;
        fs::create_directories(longPath(finalDir.parent_path()), ec);
        if (ec) {
            cleanup();
            return common::Result<application::PublishedProposalLocation>::failure(
                storeError("proposal_store.directory_failed",
                           "Could not create final proposal parent directory: " + ec.message()));
        }
    }
    {
        std::error_code ec;
        fs::rename(longPath(stagingDir), longPath(finalDir), ec);
        if (ec) {
            cleanup();
            return common::Result<application::PublishedProposalLocation>::failure(
                storeError("proposal_store.publish_failed",
                           "Could not rename staging to final: " + ec.message()));
        }
    }

    // --- Final containment check --------------------------------------------
    {
        std::error_code ec;
        const fs::path canonical = fs::canonical(longPath(finalDir), ec);
        if (ec || !isContainedIn(root_, canonical)) {
            // Extremely unlikely; would require a symlink race.
            std::error_code ignored;
            fs::remove_all(longPath(finalDir), ignored);
            return common::Result<application::PublishedProposalLocation>::failure(
                storeError("proposal_store.path_escape",
                           "Published path resolved outside the store root"));
        }
    }

    application::PublishedProposalLocation loc;
    loc.artifactRelativeRoot = relRoot;
    loc.policySnapshotHash   = policyHash;
    return common::Result<application::PublishedProposalLocation>::ok(std::move(loc));
}

common::Result<std::string> FileChangeProposalStore::readOriginalContent(
    const std::string& artifactRelativeRoot,
    const application::ChangeId& changeId) const
{
    std::lock_guard lock(mutex_);

    if (artifactRelativeRoot.empty() || changeId.empty()) {
        return common::Result<std::string>::failure(
            storeError("proposal_store.invalid_id", "artifactRelativeRoot and changeId must not be empty"));
    }

    const std::string safeId = application::sha256Hex(changeId);
    const fs::path target = root_ / fs::path(artifactRelativeRoot) / "originals" / safeId;

    // Re-validate containment. canonical() gets the extended-length form: with the plain path its failure on a long path was
    // reported below as "not found" although the file was there.
    std::error_code ec;
    const fs::path canonical = fs::canonical(longPath(target), ec);
    if (ec) {
        return common::Result<std::string>::failure(
            storeError("proposal.not_found", "Original content not found for changeId"));
    }
    if (!isContainedIn(root_, canonical)) {
        return common::Result<std::string>::failure(
            storeError("proposal_store.path_escape", "Content path escapes store root"));
    }

    return readFile(canonical);
}

common::Result<std::string> FileChangeProposalStore::readProposedContent(
    const std::string& artifactRelativeRoot,
    const application::ChangeId& changeId) const
{
    std::lock_guard lock(mutex_);

    if (artifactRelativeRoot.empty() || changeId.empty()) {
        return common::Result<std::string>::failure(
            storeError("proposal_store.invalid_id", "artifactRelativeRoot and changeId must not be empty"));
    }

    const std::string safeId = application::sha256Hex(changeId);
    const fs::path target = root_ / fs::path(artifactRelativeRoot) / "proposed" / safeId;

    std::error_code ec;
    const fs::path canonical = fs::canonical(longPath(target), ec);
    if (ec) {
        return common::Result<std::string>::failure(
            storeError("proposal.not_found", "Proposed content not found for changeId"));
    }
    if (!isContainedIn(root_, canonical)) {
        return common::Result<std::string>::failure(
            storeError("proposal_store.path_escape", "Content path escapes store root"));
    }

    return readFile(canonical);
}

} // namespace ai::infrastructure
