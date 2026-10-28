#pragma once

#include "ai/application/planning/FollowUpPlan.hpp"
#include "ai/application/planning/OutcomeBrief.hpp"
#include "ai/application/context/ProjectContextSnapshot.hpp"
#include "ai/common/Result.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace ai::application {

inline constexpr std::string_view kFollowUpComposerPromptPolicyId = "follow-up-composer.v1";
inline constexpr std::uint64_t kFollowUpComposerTotalTokens = 48'000;
inline constexpr std::uint64_t kFollowUpComposerSnapshotTokens = 24'000;
inline constexpr std::uint64_t kFollowUpComposerBriefTokens = 8'000;
inline constexpr std::uint64_t kFollowUpComposerExcerptTokens = 12'000;
inline constexpr std::uint64_t kFollowUpComposerWrapperTokens = 4'000;
inline constexpr std::uint64_t kFollowUpComposerPerFileTokens = 1'500;
inline constexpr std::size_t kFollowUpComposerMaximumFiles = 8;
// The filesystem loader normally rejects larger files before this boundary.
// The pure module repeats the limit so a non-filesystem caller cannot make
// prompt assembly hash and scan an unbounded Eligible buffer.
inline constexpr std::size_t kFollowUpComposerMaximumCandidateBytes = 256 * 1024;
// A candidate path is shown in the dialog and printed in one provider-facing header line, so it is bounded.
inline constexpr std::size_t kFollowUpComposerMaximumPathBytes = 1024;

// The future filesystem loader owns these findings. It must run the lexical
// and OS path guards, reject symlinks/reparse points, binary/invalid UTF-8 and
// run the existing secret scanner before giving content to this pure module.
// The module does not rely on that alone: this is the last pure step before a
// third-party provider, so it re-applies the lexical workspace read policy and
// the same secret scanner to what would be sent, and may only DOWNGRADE an
// Eligible candidate (to UnsafePath or SecretDetected) - never upgrade an
// exclusion. A downgraded file frees its slot and carries no provider bytes
// and no content digest.
enum class FollowUpFileSafety {
    Eligible,
    UnsafePath,
    Symlink,
    Binary,
    InvalidUtf8,
    SecretDetected,
    TooLarge,
    ReadFailed,
};

enum class FollowUpExcerptDisposition {
    Included,
    NotSelected,
    Ineligible,
    FileLimit,
};

struct FollowUpFileCandidate {
    // Printable (at most kFollowUpComposerMaximumPathBytes, valid UTF-8, no control or
    // line-break characters), canonical and workspace-relative: no leading, trailing or doubled
    // separator, no "." / ".." segment, no backslash, ':' or `* ? < > | "`. `[ ] { } , ; &` are
    // legal (route files such as pages/[id].tsx). Anything else is a loader bug and refuses the
    // whole request (follow_up.context_invalid_files); policy-denied names are not: they are
    // downgraded to UnsafePath. Unique under the workspace's case rule.
    std::string relativePath;
    FollowUpFileSafety safety {FollowUpFileSafety::ReadFailed};
    bool selected {false};
    // Present only for Eligible. sha256 names the COMPLETE, untruncated bytes.
    std::string content;
    std::string contentSha256;
};

struct FollowUpFileExcerpt {
    std::string relativePath;
    // The FINAL verdict: the candidate's own value, or UnsafePath / SecretDetected after the re-check.
    FollowUpFileSafety safety {FollowUpFileSafety::ReadFailed};
    FollowUpExcerptDisposition disposition {FollowUpExcerptDisposition::Ineligible};
    std::string contentSha256;
    std::uint64_t originalBytes {0};
    std::uint64_t shownBytes {0};
    std::uint64_t estimatedTokens {0};
    bool truncated {false};
    // Exact provider-facing block for Included; empty otherwise.
    std::string renderedText;
};

struct FollowUpComposerContextRequest {
    RunId sourceRunId;
    FollowUpPlanKind kind {FollowUpPlanKind::NextRun};
    FollowUpSplitHint splitHint {FollowUpSplitHint::Auto};
    std::uint32_t maxDrafts {1};
    std::string goal;
    ProjectContextSnapshot snapshot;
    OutcomeBrief brief;
    std::vector<FollowUpFileCandidate> files;
};

struct FollowUpComposerContext {
    std::string policyId {std::string(kFollowUpComposerPromptPolicyId)};
    std::string systemPrompt;
    std::string userContent;
    std::string contextFingerprint; // sha256 of length-framed system + user bytes
    std::uint64_t estimatedInputTokens {0};
    std::uint64_t snapshotTokens {0};
    std::uint64_t briefTokens {0};
    std::uint64_t excerptTokens {0};
    std::uint64_t wrapperTokens {0};
    bool briefTruncated {false};
    std::vector<FollowUpFileExcerpt> files; // canonical bytewise path order
};

[[nodiscard]] const char* toString(FollowUpFileSafety value) noexcept;
[[nodiscard]] const char* toString(FollowUpExcerptDisposition value) noexcept;

// The ONE fixed system prompt of policy follow-up-composer.v1 and the ONE sealing function of the exact provider bytes.
// The assembler uses both; the quote re-derives them to prove that the bytes it prices are the bytes that were sealed
// (the system prompt is the only cacheable prefix, so the quote must know it is the fixed one).
// The ONE rule for a user goal (non-empty, at most 4 KiB, valid UTF-8, no control character but LF/TAB, no Unicode line
// break): the assembler applies it to a request and the persisted follow-up request gate applies it to what it stores.
[[nodiscard]] bool followUpGoalIsValid(std::string_view goal);
// The ONE rule for a candidate path (the assembler refuses the whole request for a path that fails it, so a loader checks it
// before it emits a candidate): printable UTF-8 of at most kFollowUpComposerMaximumPathBytes, workspace-relative, '/'-separated, no
// empty, "." or ".." segment and none of `\ : * ? < > | "`.
[[nodiscard]] bool followUpCandidatePathIsCanonical(std::string_view path);
// The uniqueness key of a candidate path: its UTF-8 bytes, ASCII-lowercased where the workspace is case-insensitive (Windows).
[[nodiscard]] std::string followUpCandidatePathIdentity(std::string_view path);
[[nodiscard]] const std::string& followUpComposerSystemPrompt();
[[nodiscard]] std::string followUpComposerContextFingerprint(std::string_view systemPrompt, std::string_view userContent);

// Pure PC-5A-2 assembly: no repository, filesystem, clock, provider or Qt.
// Failure codes are stable and start with follow_up.context_.
[[nodiscard]] common::Result<FollowUpComposerContext> assembleFollowUpComposerContext(
    const FollowUpComposerContextRequest& request);
// The same, consuming the request: the candidate files (up to 64 MiB of bytes) are MOVED in, not copied; `request.files` is left empty.
// What the assembler returns is identical to the const& overload.
[[nodiscard]] common::Result<FollowUpComposerContext> assembleFollowUpComposerContext(
    FollowUpComposerContextRequest&& request);

} // namespace ai::application
