#include "ai/application/ManifestProposalOverlay.hpp"

#include "ai/application/FilesystemPathUtils.hpp"
#include "ai/application/IdGenerator.hpp"
#include "ai/application/ManifestProposalContentHash.hpp"
#include "ai/application/Sha256.hpp"

#include <algorithm>
#include <cwctype>
#include <fstream>
#include <sstream>
#include <system_error>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#endif

namespace ai::application {

namespace fs = std::filesystem;

namespace {

template <typename T>
common::Result<T> fail(std::string code, std::string message)
{
    return common::Result<T>::failure({std::move(code), std::move(message)});
}

bool isReparsePoint(const fs::path& path)
{
#ifdef _WIN32
    const auto attributes = GetFileAttributesW(detail::longPath(path).c_str());
    return attributes != INVALID_FILE_ATTRIBUTES
        && (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
#else
    std::error_code error;
    return fs::is_symlink(fs::symlink_status(detail::longPath(path), error));
#endif
}

bool safeRelative(const fs::path& path)
{
    if (path.empty() || path.is_absolute() || path.has_root_name()
        || path.has_root_directory()) return false;
    const auto value = detail::genericUtf8(path);
    if (value.empty() || value.front() == '/' || value.back() == '/'
        || value.find('\\') != std::string::npos
        || value.find(':') != std::string::npos) return false;
    for (const auto& part : path) {
        if (part.empty() || part == "." || part == "..") return false;
    }
    return true;
}

bool sha256String(std::string_view value)
{
    return value.size() == 64
        && std::all_of(value.begin(), value.end(), [](unsigned char ch) {
               return std::isdigit(ch) || (ch >= 'a' && ch <= 'f');
           });
}

bool pathContainsReparsePoint(const fs::path& root, const fs::path& relative)
{
    auto current = root;
    for (const auto& component : relative) {
        current /= component;
        if (isReparsePoint(current)) return true;
    }
    return false;
}

bool isWithin(const fs::path& candidate, const fs::path& parent)
{
    auto candidateIt = candidate.begin();
    for (auto parentIt = parent.begin(); parentIt != parent.end(); ++parentIt, ++candidateIt) {
        if (candidateIt == candidate.end()) return false;
#ifdef _WIN32
        auto left = candidateIt->wstring();
        auto right = parentIt->wstring();
        std::transform(left.begin(), left.end(), left.begin(), ::towlower);
        std::transform(right.begin(), right.end(), right.begin(), ::towlower);
        if (left != right) return false;
#else
        if (*candidateIt != *parentIt) return false;
#endif
    }
    return true;
}

common::Result<std::string> readBounded(
    const fs::path& path, std::size_t maxBytes)
{
    std::error_code error;
    const auto size = fs::file_size(detail::longPath(path), error);
    if (error || size > maxBytes) return fail<std::string>(
        "overlay.baseline_limit", "Candidate workspace exceeds its byte limit");
    std::ifstream stream(detail::longPath(path), std::ios::binary);
    if (!stream) return fail<std::string>(
        "overlay.read_failed", "Candidate workspace file could not be read");
    std::string content(static_cast<std::size_t>(size), '\0');
    if (!content.empty()) stream.read(content.data(), static_cast<std::streamsize>(content.size()));
    if (!stream && !content.empty()) return fail<std::string>(
        "overlay.read_failed", "Candidate workspace file could not be read completely");
    return common::Result<std::string>::ok(std::move(content));
}

common::Result<void> writeFile(const fs::path& path, std::string_view content)
{
    std::error_code error;
    fs::create_directories(detail::longPath(path.parent_path()), error);
    if (error) return common::Result<void>::failure(
        {"overlay.write_failed", "Candidate parent directory could not be created"});
    std::ofstream stream(detail::longPath(path), std::ios::binary | std::ios::trunc);
    if (!stream) return common::Result<void>::failure(
        {"overlay.write_failed", "Candidate file could not be opened"});
    stream.write(content.data(), static_cast<std::streamsize>(content.size()));
    if (!stream) return common::Result<void>::failure(
        {"overlay.write_failed", "Candidate file could not be written"});
    return common::Result<void>::ok();
}

common::Result<std::pair<std::string, std::pair<std::size_t, std::size_t>>>
fingerprintTree(const fs::path& root, const ManifestProposalOverlayLimits& limits)
{
    std::vector<fs::path> files;
    std::size_t totalBytes = 0;
    std::error_code error;
    for (fs::recursive_directory_iterator iterator(
             detail::longPath(root), fs::directory_options::skip_permission_denied, error), end;
         iterator != end; iterator.increment(error)) {
        if (error) return fail<std::pair<std::string, std::pair<std::size_t, std::size_t>>>(
            "overlay.scan_failed", "Candidate workspace could not be scanned");
        const auto relative = iterator->path().lexically_relative(detail::longPath(root));
        if (isReparsePoint(iterator->path())) return fail<std::pair<std::string,
            std::pair<std::size_t, std::size_t>>>("overlay.reparse_point",
            "Candidate workspace contains a symlink or reparse point");
        if (iterator->is_regular_file(error)) {
            files.push_back(relative);
            const auto size = iterator->file_size(error);
            if (error || size > limits.maxTotalBytes - std::min(
                    limits.maxTotalBytes, totalBytes)) {
                return fail<std::pair<std::string, std::pair<std::size_t, std::size_t>>>(
                    "overlay.baseline_limit", "Candidate workspace exceeds its byte limit");
            }
            totalBytes += static_cast<std::size_t>(size);
            if (files.size() > limits.maxFiles || totalBytes > limits.maxTotalBytes) {
                return fail<std::pair<std::string, std::pair<std::size_t, std::size_t>>>(
                    "overlay.baseline_limit", "Candidate workspace exceeds its configured limits");
            }
        }
    }
    std::sort(files.begin(), files.end(), [](const fs::path& left, const fs::path& right) {
        return detail::genericUtf8(left) < detail::genericUtf8(right);
    });
    std::ostringstream material;
    for (const auto& relative : files) {
        const auto content = readBounded(root / relative, limits.maxTotalBytes);
        if (!content.success()) return fail<std::pair<std::string,
            std::pair<std::size_t, std::size_t>>>(
                content.error()->code, content.error()->message);
        material << detail::genericUtf8(relative) << '\0' << content.value().size()
                 << '\0' << sha256Hex(content.value()) << '\n';
    }
    return common::Result<std::pair<std::string, std::pair<std::size_t, std::size_t>>>::ok(
        {sha256Hex(material.str()), {files.size(), totalBytes}});
}

// Manifest verification reliability hardening: the short, Backend-owned
// candidate-directory leaf prefix. Kept deliberately short - the scratch
// root itself ("aio-v", see ExecutionManifestExecutor.cpp's two default
// trustedScratchRoot values) already provides strong namespacing, so this
// leaf prefix only needs to be unambiguous within that root, not
// descriptive on its own.
constexpr std::string_view kOverlayDirectoryPrefix = "m-";
// Bounded retry budget for the (extremely unlikely, ~2^-64 per attempt)
// case where a freshly generated short token collides with an existing
// directory - see the creation loop in build() below. Never silently
// reuses an existing directory: create_directory()'s own return value (not
// just its error_code) is checked on every attempt.
constexpr int kMaxOverlayDirectoryAttempts = 8;

common::Result<void> removeKnownOverlay(
    const fs::path& root, const fs::path& scratch)
{
    std::error_code error;
    const auto canonicalScratch = detail::withoutLongPrefix(fs::weakly_canonical(detail::longPath(scratch), error));
    if (error) return common::Result<void>::failure(
        {"overlay.cleanup_denied", "Trusted overlay scratch root could not be resolved"});
    const auto canonicalRoot = detail::withoutLongPrefix(fs::weakly_canonical(detail::longPath(root), error));
    if (error || canonicalRoot.parent_path() != canonicalScratch
        || !detail::genericUtf8(canonicalRoot.filename()).starts_with(kOverlayDirectoryPrefix)) {
        return common::Result<void>::failure(
            {"overlay.cleanup_denied", "Overlay cleanup target failed containment verification"});
    }
    fs::remove_all(detail::longPath(canonicalRoot), error);
    if (error) return common::Result<void>::failure(
        {"overlay.cleanup_failed", "Overlay cleanup did not complete"});
    return common::Result<void>::ok();
}

} // namespace

common::Result<ManifestProposalOverlayDescriptor> ManifestProposalOverlay::build(
    const BuildManifestProposalOverlayRequest& request,
    ChangeProposalService& proposals)
{
    if (request.workspaceIdentity.empty() || request.orderedProposalIds.size()
        > request.limits.maxProposals || request.limits.maxFiles == 0
        || request.limits.maxTotalBytes == 0
        || request.baselineSnapshot.schemaVersion
            != kWorkspaceBaselineSchemaVersion
        || !sha256String(request.baselineSnapshot.fingerprint)) {
        return fail<ManifestProposalOverlayDescriptor>("overlay.invalid_request",
            "Candidate overlay request is incomplete or outside configured limits");
    }
    if (request.baselineSnapshot.files.size() > request.limits.maxFiles
        || request.baselineSnapshot.totalBytes > request.limits.maxTotalBytes) {
        return fail<ManifestProposalOverlayDescriptor>("overlay.baseline_limit",
            "Candidate workspace exceeds its configured limits");
    }
    std::error_code error;
    const auto workspace = detail::withoutLongPrefix(fs::canonical(detail::longPath(request.workspaceRoot), error));
    if (error || !fs::is_directory(detail::longPath(workspace), error) || isReparsePoint(workspace)) {
        return fail<ManifestProposalOverlayDescriptor>("overlay.invalid_workspace",
            "Workspace root is unavailable or unsafe");
    }
    const auto policyValidation =
        WorkspaceReadPolicyValidator::validateTrustedPolicy(request.baselinePolicy);
    const auto policyRoot = detail::withoutLongPrefix(fs::canonical(
        detail::longPath(request.baselinePolicy.workspaceRoot), error));
    if (!policyValidation.success() || error || policyRoot != workspace) {
        return fail<ManifestProposalOverlayDescriptor>("overlay.invalid_baseline_policy",
            "Trusted baseline policy does not identify the candidate workspace");
    }
    WorkspaceBaselineLimits baselineLimits;
    baselineLimits.maxFiles = request.limits.maxFiles;
    baselineLimits.maxTotalBytes = request.limits.maxTotalBytes;
    const auto currentBaseline = WorkspaceBaselineProvenance::capture(
        request.baselinePolicy, baselineLimits);
    if (!currentBaseline.success()) {
        return fail<ManifestProposalOverlayDescriptor>(
            currentBaseline.error()->code == "workspace_baseline.reparse_point"
                ? "overlay.reparse_point" : "overlay.baseline_unavailable",
            "Current workspace could not be matched to the trusted source snapshot");
    }
    if (currentBaseline.value().fingerprint
            != request.baselineSnapshot.fingerprint
        || currentBaseline.value().exclusions
            != request.baselineSnapshot.exclusions) {
        return fail<ManifestProposalOverlayDescriptor>("overlay.baseline_mismatch",
            "Workspace baseline no longer matches the trusted source snapshot");
    }
    fs::create_directories(detail::longPath(request.trustedScratchRoot), error);
    // Manifest verification reliability hardening: fs::canonical() below
    // transparently follows a symlink/reparse point to its real target, so
    // checking isReparsePoint() only on ITS result could never actually
    // detect a symlinked trustedScratchRoot - it always inspected the
    // already-resolved directory, which is never itself a reparse point.
    // Check the caller-provided path first, before it is ever resolved.
    if (isReparsePoint(request.trustedScratchRoot)) {
        return fail<ManifestProposalOverlayDescriptor>("overlay.invalid_scratch_root",
            "Trusted overlay scratch root is unavailable or unsafe");
    }
    const auto scratch = detail::withoutLongPrefix(fs::canonical(detail::longPath(request.trustedScratchRoot), error));
    if (error || isReparsePoint(scratch)) {
        return fail<ManifestProposalOverlayDescriptor>("overlay.invalid_scratch_root",
            "Trusted overlay scratch root is unavailable or unsafe");
    }
    if (isWithin(scratch, workspace) || isWithin(workspace, scratch)) {
        return fail<ManifestProposalOverlayDescriptor>("overlay.scratch_overlap",
            "Overlay scratch root must be disjoint from the real workspace");
    }
    // Manifest verification reliability hardening: create_directory()
    // returns false (not an error_code) when the target already exists -
    // silently proceeding in that case would let two independent Runs, or
    // two dependency-overlay builds within the same Run, collide onto the
    // SAME directory instead of each getting its own. Every attempt
    // therefore checks the boolean return explicitly and retries with a
    // freshly generated token rather than ever reusing an existing
    // directory.
    fs::path overlay;
    bool overlayCreated = false;
    for (int attempt = 0; attempt < kMaxOverlayDirectoryAttempts && !overlayCreated; ++attempt) {
        overlay = scratch / (std::string(kOverlayDirectoryPrefix) + generateShortToken());
        std::error_code createError;
        overlayCreated = fs::create_directory(detail::longPath(overlay), createError);
        if (createError) return fail<ManifestProposalOverlayDescriptor>("overlay.create_failed",
            "Candidate overlay directory could not be created");
    }
    if (!overlayCreated) return fail<ManifestProposalOverlayDescriptor>(
        "overlay.create_failed",
        "Could not allocate a unique candidate overlay directory after repeated attempts");

    ManifestProposalOverlayDescriptor descriptor;
    descriptor.root = overlay;
    descriptor.trustedScratchRoot = scratch;
    descriptor.workspaceIdentity = request.workspaceIdentity;
    descriptor.orderedProposalIds = request.orderedProposalIds;
    auto failWithCleanup = [&](std::string code, std::string message) {
        static_cast<void>(removeKnownOverlay(overlay, scratch));
        return fail<ManifestProposalOverlayDescriptor>(std::move(code), std::move(message));
    };

    // Materialize exactly the immutable policy-visible snapshot captured by
    // ManifestWorkflowRunner. Never enumerate the workspace here: doing so
    // would reintroduce denied generated trees and relocated CMake caches.
    std::ostringstream baselineMaterial;
    std::uint64_t baselineBytes = 0;
    std::string previousPath;
    for (const auto& entry : request.baselineSnapshot.files) {
        // The snapshot stores UTF-8 text: building the path from the narrow string would read it through the ANSI code page.
        const fs::path relative = detail::pathFromUtf8(entry.relativePath);
        if (!safeRelative(relative)
            || entry.relativePath.find('\\') != std::string::npos
            || !sha256String(entry.sha256)
            || (!previousPath.empty() && entry.relativePath <= previousPath)
            || entry.byteSize > request.limits.maxTotalBytes - (std::min)(
                static_cast<std::uint64_t>(request.limits.maxTotalBytes), baselineBytes)) {
            return failWithCleanup("overlay.invalid_baseline_snapshot",
                "Trusted workspace baseline snapshot is malformed or outside configured limits");
        }
        previousPath = entry.relativePath;
        baselineBytes += entry.byteSize;
        const auto source = workspace / relative;
        if (pathContainsReparsePoint(workspace, relative)) {
            return failWithCleanup("overlay.reparse_point",
                "Workspace baseline contains a symlink or reparse point");
        }
        const auto content = readBounded(source, request.limits.maxTotalBytes);
        if (!content.success() || content.value().size() != entry.byteSize
            || sha256Hex(content.value()) != entry.sha256) {
            return failWithCleanup("overlay.baseline_mismatch",
                "Workspace baseline no longer matches the trusted source snapshot");
        }
        const auto written = writeFile(overlay / relative, content.value());
        if (!written.success()) return failWithCleanup(
            written.error()->code, written.error()->message);
        baselineMaterial << entry.relativePath << '\0' << entry.byteSize
                         << '\0' << entry.sha256 << '\n';
    }
    if (baselineBytes != request.baselineSnapshot.totalBytes
        || sha256Hex(baselineMaterial.str())
            != request.baselineSnapshot.fingerprint) {
        return failWithCleanup("overlay.invalid_baseline_snapshot",
            "Trusted workspace baseline snapshot identity is inconsistent");
    }
    descriptor.baselineFingerprint = request.baselineSnapshot.fingerprint;

    std::size_t totalChanges = 0;
    for (const auto& proposalId : request.orderedProposalIds) {
        const auto proposal = proposals.findChangeSet(proposalId);
        if (!proposal.success() || proposal.value().state != ChangeSetState::ReadyForReview) {
            return failWithCleanup("overlay.proposal_unavailable",
                "A required proposal is missing or not ReadyForReview");
        }
        const auto changes = proposals.listChanges(proposalId);
        if (!changes.success() || totalChanges + changes.value().size()
            > request.limits.maxChanges) {
            return failWithCleanup("overlay.change_limit",
                "Proposal changes are unavailable or exceed the configured limit");
        }
        totalChanges += changes.value().size();
        std::vector<ManifestProposalContentHashChangeInput> hashInputs;
        hashInputs.reserve(changes.value().size());
        for (const auto& change : changes.value()) {
            if (!safeRelative(change.relativePath)
                || (change.sourcePath.has_value() && !safeRelative(*change.sourcePath))) {
                return failWithCleanup("overlay.invalid_change_path",
                    "Proposal contains an unsafe workspace-relative path");
            }
            const auto target = overlay / change.relativePath;
            if (isReparsePoint(target.parent_path())) return failWithCleanup(
                "overlay.reparse_point", "Proposal target crosses a reparse point");
            ManifestProposalContentHashChangeInput hashInput;
            hashInput.operation = change.operation;
            hashInput.relativePath = change.relativePath;
            hashInput.sourcePath = change.sourcePath;
            switch (change.operation) {
            case FileChangeOperation::Create:
            case FileChangeOperation::Modify: {
                const bool exists = fs::exists(detail::longPath(target), error);
                if (error || (change.operation == FileChangeOperation::Create && exists)
                    || (change.operation == FileChangeOperation::Modify && !exists)) {
                    return failWithCleanup("overlay.precondition_failed",
                        "Proposal operation does not match the candidate filesystem state");
                }
                const auto content = proposals.readProposedContent(
                    proposalId, change.changeId);
                if (!content.success()) return failWithCleanup(
                    "overlay.proposed_content_unavailable",
                    "Proposal content could not be loaded");
                const auto written = writeFile(target, content.value());
                if (!written.success()) return failWithCleanup(
                    written.error()->code, written.error()->message);
                hashInput.contentSha256 = sha256Hex(content.value());
                break;
            }
            case FileChangeOperation::Delete:
                if (!fs::is_regular_file(detail::longPath(target), error) || !fs::remove(detail::longPath(target), error)
                    || error) return failWithCleanup("overlay.precondition_failed",
                    "Delete proposal target is unavailable in the candidate workspace");
                break;
            case FileChangeOperation::Rename: {
                const auto source = overlay / *change.sourcePath;
                if (!fs::is_regular_file(detail::longPath(source), error) || fs::exists(detail::longPath(target), error)
                    || error) return failWithCleanup("overlay.precondition_failed",
                    "Rename proposal source/target precondition failed");
                fs::create_directories(detail::longPath(target.parent_path()), error);
                if (!error) fs::rename(detail::longPath(source), detail::longPath(target), error);
                if (error) return failWithCleanup("overlay.write_failed",
                    "Rename proposal could not be materialized");
                break;
            }
            }
            hashInputs.push_back(std::move(hashInput));
        }
        // AG-16.1: the single canonical algorithm, shared with
        // ManifestApplyVerificationGate so both sides can never drift - see
        // ManifestProposalContentHash.hpp's own doc comment.
        descriptor.orderedProposalContentHashes.push_back(
            computeManifestProposalContentHash(proposalId, hashInputs));
    }

    const auto finalTree = fingerprintTree(overlay, request.limits);
    if (!finalTree.success()) return failWithCleanup(
        finalTree.error()->code, finalTree.error()->message);
    descriptor.overlayFingerprint = finalTree.value().first;
    descriptor.fileCount = finalTree.value().second.first;
    descriptor.totalBytes = finalTree.value().second.second;
    return common::Result<ManifestProposalOverlayDescriptor>::ok(
        std::move(descriptor));
}

common::Result<void> ManifestProposalOverlay::cleanup(
    const ManifestProposalOverlayDescriptor& descriptor)
{
    return removeKnownOverlay(descriptor.root, descriptor.trustedScratchRoot);
}

} // namespace ai::application
