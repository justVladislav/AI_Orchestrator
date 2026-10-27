#include "ai/infrastructure/FileArtifactStore.hpp"

#include "ai/application/artifacts/ArtifactPathPolicy.hpp"
#include "ai/application/FilesystemPathUtils.hpp"
#include "ai/application/Sha256.hpp"

#include <fstream>
#include <utility>

namespace ai::infrastructure {

namespace {

namespace fs = std::filesystem;

// Every filesystem call of this store (std::filesystem AND stream opens) goes through longPath(): an artifact path is
// <root>/<run id>/<ms>-<n>-<name>, so a long profile path alone can pass the 260-character limit. See
// ai::application::detail::longPath. The containment checks below are unchanged and compare ordinary spellings.
using ai::application::detail::longPath;
using ai::application::detail::withoutLongPrefix;

common::Error error(std::string code, std::string message)
{
    return {std::move(code), std::move(message)};
}

// True when candidate's path elements start with every element of root, in
// order. Both paths must already be in canonical form; this is the
// containment check every write/resolve re-runs so a sanitized name that
// still somehow resolved outside root (or a symlink planted inside root)
// is rejected instead of silently followed.
bool isContainedIn(const fs::path& root, const fs::path& candidate)
{
    const fs::path plainRoot = withoutLongPrefix(root);
    const fs::path plainCandidate = withoutLongPrefix(candidate);
    auto rootIt = plainRoot.begin();
    auto candidateIt = plainCandidate.begin();
    for (; rootIt != plainRoot.end(); ++rootIt, ++candidateIt) {
        if (candidateIt == plainCandidate.end() || *candidateIt != *rootIt) {
            return false;
        }
    }
    return true;
}

} // namespace

FileArtifactStore::FileArtifactStore(std::filesystem::path canonicalRoot)
    : root_(std::move(canonicalRoot))
{
}

common::Result<std::unique_ptr<FileArtifactStore>> FileArtifactStore::open(
    const std::filesystem::path& root)
{
    if (root.empty()) {
        return common::Result<std::unique_ptr<FileArtifactStore>>::failure(
            error("artifact_store.empty_root", "Artifact store root cannot be empty"));
    }

    std::error_code createError;
    fs::create_directories(longPath(root), createError);
    if (createError) {
        return common::Result<std::unique_ptr<FileArtifactStore>>::failure(error(
            "artifact_store.directory_failed",
            "Could not create artifact store root: " + createError.message()));
    }

    std::error_code canonicalError;
    fs::path canonicalRoot = withoutLongPrefix(fs::canonical(longPath(root), canonicalError));
    if (canonicalError) {
        return common::Result<std::unique_ptr<FileArtifactStore>>::failure(error(
            "artifact_store.canonicalize_failed",
            "Could not canonicalize artifact store root: " + canonicalError.message()));
    }

    return common::Result<std::unique_ptr<FileArtifactStore>>::ok(
        std::unique_ptr<FileArtifactStore>(new FileArtifactStore(std::move(canonicalRoot))));
}

common::Result<application::StoredArtifactFile> FileArtifactStore::writeArtifact(
    const application::RunId& runId,
    std::string_view suggestedName,
    std::string_view content)
{
    std::uint64_t uniqueCounter = 0;
    {
        std::lock_guard lock(mutex_);
        uniqueCounter = counter_++;
    }
    const std::string relative = application::buildArtifactRelativePath(runId, suggestedName, uniqueCounter);
    const fs::path relativePath(relative);
    const fs::path target = root_ / relativePath;

    std::error_code dirError;
    fs::create_directories(longPath(target.parent_path()), dirError);
    if (dirError) {
        return common::Result<application::StoredArtifactFile>::failure(error(
            "artifact_store.directory_failed",
            "Could not create artifact directory: " + dirError.message()));
    }

    // Re-check containment against the parent directory (the file itself
    // does not exist yet, so canonical() cannot be applied to it directly).
    // relativePath is built only from sanitized segments with no ".."
    // component, so this should always hold; the check exists to fail
    // closed instead of silently trusting that invariant forever.
    std::error_code parentCanonicalError;
    const fs::path canonicalParent = fs::canonical(longPath(target.parent_path()), parentCanonicalError);
    if (parentCanonicalError || !isContainedIn(root_, canonicalParent)) {
        return common::Result<application::StoredArtifactFile>::failure(
            error("artifact_store.path_escape", "Artifact path resolved outside the artifact store root"));
    }

    // Write to a temp file in the same directory, then rename into place, so
    // a concurrent reader (or a crash mid-write) never observes a partially
    // written artifact under the final name.
    const fs::path tempTarget = target.parent_path()
        / (target.filename().string() + "." + std::to_string(uniqueCounter) + ".tmp");
    {
        std::ofstream out(longPath(tempTarget), std::ios::binary | std::ios::trunc);
        if (!out) {
            return common::Result<application::StoredArtifactFile>::failure(
                error("artifact_store.write_failed", "Could not open temporary artifact file"));
        }
        out.write(content.data(), static_cast<std::streamsize>(content.size()));
        if (!out) {
            std::error_code ignored;
            fs::remove(longPath(tempTarget), ignored);
            return common::Result<application::StoredArtifactFile>::failure(
                error("artifact_store.write_failed", "Could not write artifact content"));
        }
    }

    std::error_code renameError;
    fs::rename(longPath(tempTarget), longPath(target), renameError);
    if (renameError) {
        std::error_code ignored;
        fs::remove(longPath(tempTarget), ignored);
        return common::Result<application::StoredArtifactFile>::failure(error(
            "artifact_store.write_failed",
            "Could not finalize artifact file: " + renameError.message()));
    }

    // Final defense-in-depth check on the file that now actually exists.
    std::error_code finalCanonicalError;
    const fs::path canonicalTarget = fs::canonical(longPath(target), finalCanonicalError);
    if (finalCanonicalError || !isContainedIn(root_, canonicalTarget)) {
        std::error_code ignored;
        fs::remove(longPath(target), ignored);
        return common::Result<application::StoredArtifactFile>::failure(
            error("artifact_store.path_escape", "Artifact path resolved outside the artifact store root"));
    }

    application::StoredArtifactFile stored;
    stored.relativePath = relativePath;
    stored.byteSize = content.size();
    stored.sha256 = application::sha256Hex(content);
    return common::Result<application::StoredArtifactFile>::ok(std::move(stored));
}

common::Result<std::filesystem::path> FileArtifactStore::resolveAbsolutePath(
    const std::filesystem::path& relativePath) const
{
    if (relativePath.empty() || relativePath.is_absolute()) {
        return common::Result<std::filesystem::path>::failure(
            error("artifact_store.invalid_path", "Artifact relative path must be a non-empty relative path"));
    }

    const fs::path candidate = root_ / relativePath;
    std::error_code existsError;
    if (!fs::exists(longPath(candidate), existsError) || existsError) {
        return common::Result<std::filesystem::path>::failure(
            error("artifact.not_found", "Artifact content file was not found"));
    }

    std::error_code canonicalError;
    const fs::path canonicalCandidate = fs::canonical(longPath(candidate), canonicalError);
    if (canonicalError) {
        return common::Result<std::filesystem::path>::failure(
            error("artifact.not_found", "Could not resolve artifact content file"));
    }

    if (!isContainedIn(root_, canonicalCandidate)) {
        // Re-validated at read time as well as write time: a stored path
        // must never be trusted just because it was safe when written, in
        // case something inside the root was replaced (e.g. by a symlink)
        // afterward.
        return common::Result<std::filesystem::path>::failure(
            error("artifact_store.path_escape", "Artifact path resolved outside the artifact store root"));
    }

    return common::Result<std::filesystem::path>::ok(canonicalCandidate);
}

common::Result<std::string> FileArtifactStore::readArtifact(
    const std::filesystem::path& relativePath) const
{
    const auto resolved = resolveAbsolutePath(relativePath);
    if (!resolved.success()) {
        return common::Result<std::string>::failure(*resolved.error());
    }

    std::ifstream in(longPath(resolved.value()), std::ios::binary);
    if (!in) {
        return common::Result<std::string>::failure(
            error("artifact_store.read_failed", "Could not open artifact content file"));
    }

    std::string content(
        (std::istreambuf_iterator<char>(in)),
        std::istreambuf_iterator<char>());
    if (in.bad()) {
        return common::Result<std::string>::failure(
            error("artifact_store.read_failed", "Could not read artifact content file"));
    }
    return common::Result<std::string>::ok(std::move(content));
}

common::Result<void> FileArtifactStore::removeArtifact(
    const std::filesystem::path& relativePath)
{
    // Never remove a path produced by raw concatenation. Resolve performs the
    // existence, canonicalization, and root-containment checks first.
    const auto resolved = resolveAbsolutePath(relativePath);
    if (!resolved.success()) {
        return common::Result<void>::failure(*resolved.error());
    }

    std::error_code removeError;
    if (!fs::remove(longPath(resolved.value()), removeError) || removeError) {
        return common::Result<void>::failure(error(
            "artifact_store.remove_failed",
            "Could not remove artifact after metadata failure: " + removeError.message()));
    }
    return common::Result<void>::ok();
}

} // namespace ai::infrastructure
