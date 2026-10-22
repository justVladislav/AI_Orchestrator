#include "ai/application/ChangeProposalValidator.hpp"

#include "ai/application/FilesystemPathUtils.hpp"
#include "ai/application/Sha256.hpp"

#include <algorithm>
#include <cctype>
#include <map>
#include <regex>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace ai::application {

namespace {

namespace fs = std::filesystem;

ValidationFinding finding(std::string code, std::string message, std::string changeId = {})
{
    ValidationFinding f;
    f.code     = std::move(code);
    f.message  = std::move(message);
    f.changeId = std::move(changeId);
    return f;
}

std::string normalizedPathComponent(std::string value)
{
#ifdef _WIN32
    value = detail::asciiLower(std::move(value));
#endif
    return value;
}

bool pathComponentEquals(const fs::path& left, const fs::path& right)
{
    return normalizedPathComponent(detail::genericUtf8(left))
        == normalizedPathComponent(detail::genericUtf8(right));
}

bool pathStartsWith(const fs::path& path, const fs::path& prefix)
{
    if (prefix.empty()) return true;
    const fs::path normalizedPath = path.lexically_normal();
    const fs::path normalizedPrefix = prefix.lexically_normal();
    auto pathIt = normalizedPath.begin();
    const auto pathEnd = normalizedPath.end();
    for (const auto& component : normalizedPrefix) {
        if (pathIt == pathEnd || !pathComponentEquals(*pathIt, component)) {
            return false;
        }
        ++pathIt;
    }
    return true;
}

// Returns true if the path must be rejected as non-relative:
// - absolute per std::filesystem (handles POSIX and Windows fully-qualified)
// - starts with a drive letter + colon (C:foo on Windows)
// - starts with / or \ (root-relative on Windows, absolute on POSIX;
//   this catches /etc/passwd which is_absolute() misses on Windows)
bool isAbsoluteOrDriveQualified(const fs::path& p)
{
    if (p.is_absolute()) return true;
    const std::string s = detail::genericUtf8(p);
    if (s.size() >= 2 && std::isalpha(static_cast<unsigned char>(s[0])) && s[1] == ':') return true;
    if (!s.empty() && (s[0] == '/' || s[0] == '\\')) return true;
    return false;
}

// True if any element of the path is ".."
bool hasTraversal(const fs::path& p)
{
    for (const auto& part : p) {
        if (part == "..") return true;
    }
    return false;
}

// True if the string representation contains a NUL byte.
bool hasNul(const fs::path& p)
{
    const std::string s = detail::genericUtf8(p);
    return s.find('\0') != std::string::npos;
}

// True if any path component contains a colon (Windows ADS: file.txt:zone.id).
bool hasAds(const fs::path& p)
{
    for (const auto& part : p) {
        const std::string s = detail::genericUtf8(part);
        // Drive-qualified paths were rejected before this check, so any
        // remaining colon is an ADS/device-path separator on Windows.
        if (s.find(':') != std::string::npos) return true;
    }
    return false;
}

// True if the path starts with ".git" as the first component or is inside it.
bool isGitPath(const fs::path& p)
{
    const auto it = p.begin();
    if (it == p.end()) return false;
    return normalizedPathComponent(detail::genericUtf8(*it)) == ".git";
}

// True if the filename looks like a secret/credential/env file.
bool isSecretFilename(const fs::path& p)
{
    const std::string name = normalizedPathComponent(detail::genericUtf8(p.filename()));
    // Check for common secret-bearing filenames.
    static const char* const secretNames[] = {
        ".env", ".env.local", ".env.production", ".env.development",
        ".env.test", ".envrc", "credentials", "credentials.json",
        "secrets.json", "secrets.yaml", "secrets.yml", ".netrc",
        ".htpasswd", "id_rsa", "id_ed25519", "id_ecdsa", "id_dsa",
        "private_key", "private.pem", "private.key",
    };
    for (const char* s : secretNames) {
        if (name == s) return true;
    }
    // Files whose names contain "secret" or "credential" are suspicious.
    if (name.find("secret") != std::string::npos) return true;
    if (name.find("credential") != std::string::npos) return true;
    if (name.find("apikey") != std::string::npos) return true;
    if (name.find("api_key") != std::string::npos) return true;
    return false;
}

// True if the path starts with a build / install / system directory.
bool isBuildOrSystemPath(const fs::path& p)
{
    const auto it = p.begin();
    if (it == p.end()) return false;
    const std::string first = normalizedPathComponent(detail::genericUtf8(*it));
    static const char* const buildDirs[] = {
        "build", "out", "dist", "install", "release", "debug", "x64",
        "x86", "cmake-build-debug", "cmake-build-release", ".vs", ".idea",
        "__pycache__", "node_modules", ".gradle", "target", "bin", "obj",
        "vendor", "third_party", "thirdparty"
    };
    for (const char* d : buildDirs) {
        if (first == d) return true;
    }
    return false;
}

// Check if the path contains only valid (non-NUL, non-ADS) characters and
// has no leading dots that resolve to hidden/relative markers ("." or "..").
bool hasDotSegment(const fs::path& p)
{
    for (const auto& part : p) {
        const std::string s = detail::genericUtf8(part);
        if (s == ".") return true; // explicit current-dir segment
        // ".." is already caught by hasTraversal
    }
    return false;
}

// Checks all path safety rules and returns a finding if violated.
// Returns std::nullopt if the path passes.
std::optional<ValidationFinding> checkPathSafety(const fs::path& rel, const std::string& changeId)
{
    if (rel.empty()) {
        return finding("proposal.empty_path", "Path must not be empty", changeId);
    }
    if (isAbsoluteOrDriveQualified(rel)) {
        return finding("proposal.absolute_path",
                       "Path must be relative and not drive-qualified", changeId);
    }
    if (hasTraversal(rel)) {
        return finding("proposal.path_traversal",
                       "Path must not contain '..' components", changeId);
    }
    if (hasNul(rel)) {
        return finding("proposal.invalid_path", "Path must not contain NUL bytes", changeId);
    }
    if (hasAds(rel)) {
        return finding("proposal.ads_path",
                       "Path must not contain Alternate Data Stream separators", changeId);
    }
    if (hasDotSegment(rel)) {
        return finding("proposal.invalid_path",
                       "Path must not contain explicit '.' segments", changeId);
    }
    if (isGitPath(rel)) {
        return finding("proposal.git_path", "Path must not target the .git directory", changeId);
    }
    if (isSecretFilename(rel)) {
        return finding("proposal.secret_file",
                       "Path targets a credential or secret file", changeId);
    }
    if (isBuildOrSystemPath(rel)) {
        return finding("proposal.build_path",
                       "Path targets a build or system directory", changeId);
    }
    return std::nullopt;
}

// Checks if the path is within at least one allowedRoot (or if allowedRoots is empty).
bool isWithinAllowedRoots(
    const fs::path& rel,
    const std::vector<fs::path>& allowedRoots,
    bool exactPaths)
{
    if (allowedRoots.empty()) return true;
    for (const auto& root : allowedRoots) {
        if (root.empty()) continue;
        if (!exactPaths && pathStartsWith(rel, root)) return true;
        if (exactPaths
            && normalizedPathComponent(detail::genericUtf8(rel.lexically_normal()))
                == normalizedPathComponent(detail::genericUtf8(root.lexically_normal()))) return true;
    }
    return false;
}

// Checks if the path matches any denied path exactly or is inside a denied directory.
bool isDenied(const fs::path& rel, const std::vector<fs::path>& deniedPaths)
{
    for (const auto& denied : deniedPaths) {
        if (!denied.empty() && pathStartsWith(rel, denied)) return true;
    }
    return false;
}

// True if the extension is in the allowed list (empty list means all allowed).
bool isExtensionAllowed(const fs::path& rel, const std::vector<std::string>& allowedExtensions)
{
    if (allowedExtensions.empty()) return true;
    const std::string ext = normalizedPathComponent(detail::genericUtf8(rel.extension()));
    for (const auto& allowed : allowedExtensions) {
        if (ext == normalizedPathComponent(allowed)) return true;
    }
    return false;
}

// Check that proposedContent is valid UTF-8 text (no NUL bytes, valid encoding).
bool isValidUtf8(std::string_view text)
{
    for (std::size_t i = 0; i < text.size();) {
        const auto lead = static_cast<unsigned char>(text[i]);
        std::size_t continuationCount = 0;
        if (lead <= 0x7F) {
            ++i;
            continue;
        }
        if (lead >= 0xC2 && lead <= 0xDF) continuationCount = 1;
        else if (lead >= 0xE0 && lead <= 0xEF) continuationCount = 2;
        else if (lead >= 0xF0 && lead <= 0xF4) continuationCount = 3;
        else return false;
        if (i + continuationCount >= text.size()) return false;
        for (std::size_t j = 1; j <= continuationCount; ++j) {
            const auto continuation = static_cast<unsigned char>(text[i + j]);
            if ((continuation & 0xC0) != 0x80) return false;
        }
        if (continuationCount == 2) {
            const auto second = static_cast<unsigned char>(text[i + 1]);
            if ((lead == 0xE0 && second < 0xA0)
                || (lead == 0xED && second >= 0xA0)) return false;
        } else if (continuationCount == 3) {
            const auto second = static_cast<unsigned char>(text[i + 1]);
            if ((lead == 0xF0 && second < 0x90)
                || (lead == 0xF4 && second >= 0x90)) return false;
        }
        i += continuationCount + 1;
    }
    return true;
}

std::optional<ValidationFinding> checkBinaryContent(const ProposedFileChange& change)
{
    if (!change.proposedContent.has_value()) return std::nullopt;
    const auto& content = *change.proposedContent;
    if (content.find('\0') != std::string::npos || !isValidUtf8(content)) {
        return finding("proposal.binary_not_supported",
                       "Proposed content must be valid UTF-8 text without NUL bytes",
                       change.changeId);
    }
    return std::nullopt;
}

// The path a change's original-hash precondition logically describes:
// relativePath for Modify/Delete, sourcePath for Rename ("hash of source" -
// see checkOperationInvariants below), and no path at all for Create (which
// has no original to hash and must not carry expectedOriginalHash).
std::optional<fs::path> hashSubjectPath(const ProposedFileChange& change)
{
    switch (change.operation) {
    case FileChangeOperation::Modify:
    case FileChangeOperation::Delete:
        return change.relativePath;
    case FileChangeOperation::Rename:
        return change.sourcePath;
    case FileChangeOperation::Create:
        return std::nullopt;
    }
    return std::nullopt;
}

// observedFileHashes keys must already use this same normalization (shared
// with AgentToolTurnLoopRunner's evidence collector via detail::
// normalizedPathKey) so a lookup here matches regardless of which side built
// the key.
std::string evidenceKey(const fs::path& p)
{
    return detail::normalizedPathKey(p);
}

// A Modify/Delete/Rename original-hash precondition must be backed by an
// audited workspace_hash_file result for the exact path being changed - the
// provider-facing proposal-submission.v2 contract has no
// expected_original_hash field at all, so this can never be a free-form
// string the model typed into propose_change_set. It is instead populated by
// AgentToolTurnLoopRunner::preflightTrustedHashes, which the Backend runs
// itself (through the same audited tool-execution path a model's own call
// would use) for every Modify/Delete/Rename target before validation ever
// sees this proposal - the model is not expected or required to call
// workspace_hash_file itself. Reproduces the real incident (Run
// f5058a12-746a-4ab6-8deb-ca5bfc5500ca, from an earlier design where the
// model still supplied its own hash) where a model invented the same
// expected_original_hash for five Modify changes without ever calling
// workspace_hash_file: the pre-existing checkHash below only caught this by
// coincidence (a fabricated 64-hex-digit string almost never equals the real
// content hash), producing a generic, confusing hash_mismatch indistinguishable
// from a genuine transcription typo or an actual stale-world race. This check
// makes "no trusted hash at all" its own precise finding instead - now most
// often meaning the Backend's own preflight could not hash that path (e.g.
// it does not exist), which is model-fixable by correcting the operation or
// path, not a fact re-prompting can never change.
// Only active when requireHashEvidence is true (see validate()'s doc comment);
// a Create has no hashSubjectPath and is always exempt.
std::optional<ValidationFinding> checkHashEvidence(
    const ProposedFileChange& change,
    bool requireHashEvidence,
    const std::unordered_map<std::string, std::string>& observedFileHashes)
{
    if (!requireHashEvidence) return std::nullopt;
    const auto subject = hashSubjectPath(change);
    if (!subject.has_value()) return std::nullopt;
    if (observedFileHashes.count(evidenceKey(*subject)) != 0) return std::nullopt;
    return finding("proposal.missing_hash_evidence",
                   "No trusted hash is available for this path; the Backend could "
                   "not independently confirm its current content before validation",
                   change.changeId);
}

// Check hash of capturedOriginalContent against a trusted expected hash.
// When requireHashEvidence is true, the trusted hash comes from
// observedFileHashes (a real workspace_hash_file result this session
// actually saw for the change's hashSubjectPath), not from the change's own
// expectedOriginalHash string - see checkHashEvidence above for why a missing
// evidence entry is reported as its own finding instead of falling through
// to a mismatch here. When false (the default for callers with no session
// evidence), behaviour is unchanged from before this check existed: trust
// expectedOriginalHash and compare it directly to the real on-disk content.
std::optional<ValidationFinding> checkHash(
    const ProposedFileChange& change,
    bool requireHashEvidence,
    const std::unordered_map<std::string, std::string>& observedFileHashes)
{
    if (!change.expectedOriginalHash.has_value()) return std::nullopt;
    if (!change.capturedOriginalContent.has_value()) {
        // Service didn't snapshot → probably file doesn't exist / couldn't be read.
        // The service adds an explicit finding for that; skip hash check here.
        return std::nullopt;
    }

    std::string trustedHash;
    if (requireHashEvidence) {
        const auto subject = hashSubjectPath(change);
        if (!subject.has_value()) return std::nullopt;
        const auto it = observedFileHashes.find(evidenceKey(*subject));
        // Missing evidence is already reported by checkHashEvidence(); adding
        // a second finding here by falling back to the untrusted model
        // string would only be redundant and confusing.
        if (it == observedFileHashes.end()) return std::nullopt;
        trustedHash = it->second;
    } else {
        trustedHash = *change.expectedOriginalHash;
    }
    std::transform(trustedHash.begin(), trustedHash.end(), trustedHash.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    const std::string actualHash = sha256Hex(*change.capturedOriginalContent);
    if (actualHash != trustedHash) {
        return finding("proposal.hash_mismatch",
                       requireHashEvidence
                           ? "Observed workspace hash no longer matches actual file "
                             "content"
                           : "expectedOriginalHash does not match actual file content",
                       change.changeId);
    }
    return std::nullopt;
}

// Check operation invariants (structural rules, no filesystem access).
void checkOperationInvariants(
    const ProposedFileChange& change,
    std::vector<ValidationFinding>& out)
{
    const auto& cid = change.changeId;
    if (change.expectedOriginalHash.has_value()
        && (change.expectedOriginalHash->size() != 64
            || !std::all_of(
                change.expectedOriginalHash->begin(),
                change.expectedOriginalHash->end(),
                [](unsigned char c) { return std::isxdigit(c) != 0; }))) {
        out.push_back(finding(
            "proposal.invalid_original_hash",
            "expectedOriginalHash must be a 64-character SHA-256 hex digest",
            cid));
    }
    switch (change.operation) {
    case FileChangeOperation::Create:
        if (change.expectedOriginalHash.has_value())
            out.push_back(finding("proposal.invariant_violated",
                "Create must not have expectedOriginalHash", cid));
        if (change.sourcePath.has_value())
            out.push_back(finding("proposal.invariant_violated",
                "Create must not have sourcePath", cid));
        if (!change.proposedContent.has_value())
            // Distinct from proposal.invariant_violated: unlike the other
            // per-operation invariants, an omitted proposedContent reflects
            // nothing about real repository/policy state - it is purely a
            // shape mistake the model can correct if re-prompted, so
            // ReadOnlyAnalysisWorkflowRunner::isModelFormatError() treats
            // this code as retriable.
            out.push_back(finding("proposal.missing_proposed_content",
                "Create requires proposedContent", cid));
        break;
    case FileChangeOperation::Modify:
        if (!change.expectedOriginalHash.has_value())
            out.push_back(finding("proposal.invariant_violated",
                "Modify requires expectedOriginalHash", cid));
        if (change.sourcePath.has_value())
            out.push_back(finding("proposal.invariant_violated",
                "Modify must not have sourcePath", cid));
        if (!change.proposedContent.has_value())
            out.push_back(finding("proposal.missing_proposed_content",
                "Modify requires proposedContent", cid));
        break;
    case FileChangeOperation::Delete:
        if (!change.expectedOriginalHash.has_value())
            out.push_back(finding("proposal.invariant_violated",
                "Delete requires expectedOriginalHash", cid));
        if (change.sourcePath.has_value())
            out.push_back(finding("proposal.invariant_violated",
                "Delete must not have sourcePath", cid));
        if (change.proposedContent.has_value())
            out.push_back(finding("proposal.invariant_violated",
                "Delete must not have proposedContent", cid));
        break;
    case FileChangeOperation::Rename:
        if (!change.expectedOriginalHash.has_value())
            out.push_back(finding("proposal.invariant_violated",
                "Rename requires expectedOriginalHash (of source)", cid));
        if (!change.sourcePath.has_value())
            out.push_back(finding("proposal.invariant_violated",
                "Rename requires sourcePath", cid));
        if (change.proposedContent.has_value())
            out.push_back(finding("proposal.invariant_violated",
                "Rename must not have proposedContent", cid));
        break;
    }
}

} // namespace

std::vector<ValidationFinding> ChangeProposalValidator::validate(
    const ProposedChangeSet& changeSet,
    const ProjectWritePolicy& policy,
    const std::filesystem::path& projectRoot,
    bool requireHashEvidence,
    const std::unordered_map<std::string, std::string>& observedFileHashes)
{
    std::vector<ValidationFinding> findings;

    std::error_code rootError;
    const fs::path canonicalRoot = detail::withoutLongPrefix(fs::weakly_canonical(detail::longPath(projectRoot), rootError));
    if (!projectRoot.empty()
        && (!projectRoot.is_absolute() || rootError
            || !fs::is_directory(detail::longPath(canonicalRoot), rootError) || rootError)) {
        findings.push_back(finding(
            "proposal.invalid_project_root",
            "Project root must be an existing absolute directory"));
    }
    // An empty changes[] is not a validation failure: the agent may
    // legitimately conclude that no file changes are needed and report
    // findings only (see kProposeChangeSetInputSchema/kWorkspaceToolsSystemPrompt
    // in AgentToolTurnLoopRunner.cpp, which explicitly document this as
    // allowed). ChangeProposalService turns an empty, otherwise-clean change
    // set into ChangeSetState::NoChangesNeeded rather than Rejected.

    // 1. File count limit
    constexpr std::size_t defaultFileLimit = 50;
    constexpr std::size_t defaultByteLimit = 10ULL * 1024 * 1024;
    const std::size_t fileLimit = policy.maxFilesPerChangeSet == 0
        ? defaultFileLimit : policy.maxFilesPerChangeSet;
    const std::size_t byteLimit = policy.maxTotalBytes == 0
        ? defaultByteLimit : policy.maxTotalBytes;
    if (changeSet.changes.size() > fileLimit) {
        findings.push_back(finding("proposal.too_many_files",
            "Change set exceeds maxFilesPerChangeSet limit"));
    }

    // 2. Total byte budget
    std::size_t totalBytes = 0;
    for (const auto& ch : changeSet.changes) {
        if (ch.proposedContent.has_value())
            totalBytes += ch.proposedContent->size();
    }
    if (totalBytes > byteLimit) {
        findings.push_back(finding("proposal.too_large",
            "Total proposed content exceeds maxTotalBytes limit"));
    }

    // Per-change tracking for duplicate/conflict detection
    std::set<std::string> seenChangeIds;
    std::map<std::string, std::string> pathToFirstChangeId; // canonical-generic-path → first changeId

    for (const auto& ch : changeSet.changes) {
        // 3. Duplicate changeId
        if (!seenChangeIds.insert(ch.changeId).second) {
            findings.push_back(finding("proposal.duplicate_change_id",
                "Duplicate changeId", ch.changeId));
        }
        if (ch.changeId.size() > 128
            || !std::all_of(ch.changeId.begin(), ch.changeId.end(), [](unsigned char c) {
                return std::isalnum(c) || c == '-' || c == '_';
            })) {
            findings.push_back(finding(
                "proposal.invalid_change_id",
                "changeId must contain only letters, digits, '-' or '_'",
                ch.changeId));
        }

        // 4. Operation invariants
        checkOperationInvariants(ch, findings);

        // 5. Path safety on relativePath
        if (const auto f = checkPathSafety(ch.relativePath, ch.changeId)) {
            findings.push_back(*f);
        }

        // 6. Policy enforcement on relativePath
        const std::string relGeneric = detail::genericUtf8(ch.relativePath);
        if (!isWithinAllowedRoots(
                ch.relativePath, policy.allowedRoots, policy.allowedRootsAreExactPaths)) {
            findings.push_back(finding("proposal.outside_allowed_roots",
                "Path is outside the authorized write allocation", ch.changeId));
        }
        if (isDenied(ch.relativePath, policy.deniedPaths)) {
            findings.push_back(finding("proposal.denied_path",
                "Path matches a denied path", ch.changeId));
        }
        if (!isExtensionAllowed(ch.relativePath, policy.allowedExtensions)) {
            findings.push_back(finding("proposal.extension_not_allowed",
                "File extension is not in the allowed list", ch.changeId));
        }

        // 7. allowDelete / allowRename policy
        if (ch.operation == FileChangeOperation::Delete && !policy.allowDelete) {
            findings.push_back(finding("proposal.delete_not_permitted",
                "Delete operations are not permitted by policy", ch.changeId));
        }
        if (ch.operation == FileChangeOperation::Rename && !policy.allowRename) {
            findings.push_back(finding("proposal.rename_not_permitted",
                "Rename operations are not permitted by policy", ch.changeId));
        }

        // 8. Conflicting path operations
        if (!pathToFirstChangeId.emplace(relGeneric, ch.changeId).second) {
            findings.push_back(finding("proposal.conflicting_path",
                "Two changes target the same path", ch.changeId));
        }

        // 9. Source path safety (Rename)
        if (ch.sourcePath.has_value()) {
            if (const auto f = checkPathSafety(*ch.sourcePath, ch.changeId)) {
                findings.push_back(*f);
            }
            if (!isWithinAllowedRoots(
                    *ch.sourcePath, policy.allowedRoots, policy.allowedRootsAreExactPaths)) {
                findings.push_back(finding("proposal.outside_allowed_roots",
                    "Rename source is outside the authorized write allocation", ch.changeId));
            }
            if (isDenied(*ch.sourcePath, policy.deniedPaths)) {
                findings.push_back(finding("proposal.denied_path",
                    "Rename source matches a denied path", ch.changeId));
            }
            if (!isExtensionAllowed(*ch.sourcePath, policy.allowedExtensions)) {
                findings.push_back(finding("proposal.extension_not_allowed",
                    "Rename source extension is not in the allowed list", ch.changeId));
            }
        }

        // 10. Binary content check
        if (const auto f = checkBinaryContent(ch)) {
            findings.push_back(*f);
        }

        // 11. Hash evidence (requireHashEvidence callers only; see checkHashEvidence)
        if (const auto f = checkHashEvidence(ch, requireHashEvidence, observedFileHashes)) {
            findings.push_back(*f);
        }

        // 12. Hash verification (uses capturedOriginalContent pre-set by service)
        if (const auto f = checkHash(ch, requireHashEvidence, observedFileHashes)) {
            findings.push_back(*f);
        }
    }

    // 13. Symlink / reparse escape: re-check canonicalization inside projectRoot.
    // We do this only for changes that passed the basic path-safety checks.
    for (const auto& ch : changeSet.changes) {
        if (!rootError && !canonicalRoot.empty()) {
            auto checkResolvedPath = [&](const fs::path& relativePath) {
            const fs::path candidate = canonicalRoot / relativePath;
            std::error_code ec;
            // Resolve symlinks in the existing prefix while permitting Create
            // to target directories that will only be created during Apply.
            const fs::path canonical =
                detail::canonicalizeWithMissingSuffix(candidate, ec);
            if (ec || !pathStartsWith(canonical, canonicalRoot)) {
                findings.push_back(finding("proposal.path_escape",
                    "Path resolves outside the project root", ch.changeId));
            }
            };
            checkResolvedPath(ch.relativePath);
            if (ch.sourcePath.has_value()) {
                checkResolvedPath(*ch.sourcePath);
            }
        }
    }

    return findings;
}

std::vector<ValidationFinding> ChangeProposalValidator::scanForSecrets(
    const ProposedChangeSet& changeSet)
{
    std::vector<ValidationFinding> findings;

    // Patterns are intentionally broad; false positives are acceptable
    // (proposal requires review anyway) but false negatives are not.
    // IMPORTANT: never echo the matched value in the finding message.
    static const std::vector<std::pair<std::regex, const char*>> patterns = {
        {std::regex(R"(AKIA[A-Z0-9]{16})"),                          "AWS Access Key"},
        {std::regex(R"(sk-[a-zA-Z0-9]{32,})"),                       "OpenAI-style API key"},
        {std::regex(R"(-----BEGIN [A-Z ]*(PRIVATE KEY)-----)"),      "PEM private key"},
        {std::regex(R"(Authorization\s*:\s*Bearer\s+[A-Za-z0-9._~+/-]+=*)", std::regex::icase),
                                                                      "Bearer authorization token"},
        {std::regex(R"([A-Z_]+_API_KEY\s*=\s*[^\s'"]{8,})"),        "API key assignment"},
        {std::regex(R"(ghp_[a-zA-Z0-9]{36})"),                      "GitHub personal access token"},
        {std::regex(R"(glpat-[a-zA-Z0-9_\-]{20,})"),                "GitLab personal access token"},
        {std::regex(R"(xoxb-[0-9]+-[0-9]+-[a-zA-Z0-9]+)"),         "Slack bot token"},
        {std::regex(R"([a-f0-9]{64})"),                              "Potential secret hash/key"},
    };

    auto scan = [&](const std::string& text, const std::string& fieldName, const std::string& changeId) {
        for (const auto& [pattern, label] : patterns) {
            if (std::regex_search(text, pattern)) {
                findings.push_back(
                    finding("proposal.secret_detected",
                            std::string("Potential ") + label + " detected in " + fieldName,
                            changeId));
                // One finding per field per change — don't enumerate all matches.
                break;
            }
        }
    };

    // Scan the change-set summary.
    if (!changeSet.summary.empty()) {
        scan(changeSet.summary, "summary", "");
    }

    for (const auto& ch : changeSet.changes) {
        // Scan the relative path string representation.
        scan(detail::genericUtf8(ch.relativePath), "relative_path", ch.changeId);

        // Scan source path (Rename only).
        if (ch.sourcePath.has_value()) {
            scan(detail::genericUtf8(*ch.sourcePath), "source_path", ch.changeId);
        }

        // Scan proposed content.
        if (ch.proposedContent.has_value()) {
            scan(*ch.proposedContent, "proposed_content", ch.changeId);
        }
        if (ch.capturedOriginalContent.has_value()) {
            scan(*ch.capturedOriginalContent, "original_snapshot", ch.changeId);
        }
    }

    return findings;
}

} // namespace ai::application
