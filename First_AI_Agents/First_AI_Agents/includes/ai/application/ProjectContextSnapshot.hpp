#pragma once

#include "ai/application/ApplicationTypes.hpp"
#include "ai/common/Result.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ai::application {

// PC-1 (gap MW-P10): one immutable, bounded, fingerprinted read model of "what the Backend knows about a
// project", shared by every future consumer (Chat Prompt Workshop, Run Draft authoring, the post-resolution
// composer). This header is the pure part (PC-1A): value types, the token budget policy, deterministic
// section rendering and the canonical fingerprint. It reads no repository, no file and no clock, calls no
// provider and writes nothing; the authoritative loaders that feed it are PC-1B/1C.
inline constexpr std::string_view kProjectContextSnapshotSchemaVersion = "project-context-snapshot.v1";
inline constexpr std::string_view kProjectContextBudgetPolicyId = "project_context_budget_v1";

// Canonical section order. The assembler always emits sections in this order whatever order the caller
// supplied them in.
enum class ProjectContextCategory {
    ProjectMetadata,   // project name / description (the only persisted curated note today)
    SelectedLineage,   // the selected Run, its parent chain and manual-handoff resolution state
    WorkLedger,        // bounded recent Work Ledger facts (newest first)
    WorkspaceCatalog,  // bounded catalog of the current workspace baseline
    ProjectMemory,     // optional derived narrative, never authoritative
};

enum class ProjectContextAuthority {
    HumanCurated,   // project description: intent, never overrides the workspace or the ledger
    Authoritative,  // workspace baseline, Work Ledger, Run lineage / resolution state
    Derived,        // Project Memory: always explicitly labelled as derived
};

[[nodiscard]] const char* toString(ProjectContextCategory category) noexcept;
[[nodiscard]] const char* toString(ProjectContextAuthority authority) noexcept;
// Authority is a property of the category, never of the caller: a loader cannot label derived text as
// authoritative.
[[nodiscard]] ProjectContextAuthority projectContextAuthorityOf(ProjectContextCategory category) noexcept;
// The rendered heading line of a section, e.g. "## Work Ledger".
[[nodiscard]] std::string_view projectContextSectionHeading(ProjectContextCategory category) noexcept;

// Fixed policy "project_context_budget_v1": 24 000 estimated tokens (estimateChatTokens) in total. The
// defaults ARE the v1 limits. Production code calls assembleProjectContextSnapshot(), which always uses
// them; a different limit set is only reachable through assembleProjectContextSnapshotWithCustomBudget()
// (tests, diagnostics) and such a snapshot never carries the v1 policy id.
struct ProjectContextBudget {
    std::uint64_t totalTokens {24'000};
    std::uint64_t projectMetadataTokens {2'000};
    std::uint64_t selectedLineageTokens {5'000};
    std::uint64_t workLedgerTokens {6'000};
    std::uint64_t workspaceCatalogTokens {9'000};
    std::uint64_t projectMemoryTokens {2'000};

    [[nodiscard]] std::uint64_t forCategory(ProjectContextCategory category) const noexcept;
    [[nodiscard]] bool isV1() const noexcept { return *this == ProjectContextBudget {}; }
    [[nodiscard]] bool operator==(const ProjectContextBudget&) const noexcept = default;
};

// A stable identity of one fact, never a filesystem path. `digest` is an optional content digest or revision.
struct ProjectContextSourceRef {
    ProjectContextCategory category {ProjectContextCategory::ProjectMetadata};  // set by the assembler
    std::string kind;    // lowercase token, e.g. "project", "run", "ledger_entry", "workspace", "memory_segment"
    std::string id;
    std::string digest;
};

// One whole, indivisible rendered fact. Budget trimming removes whole items from the tail, never part of one.
struct ProjectContextItem {
    std::string text;
    ProjectContextSourceRef source;  // `category` is ignored on input
    // A mandatory item is never trimmed; if the mandatory items alone exceed the section budget the assembly
    // fails instead of dropping identity. Mandatory is NOT a caller privilege: only the three identity roles
    // may carry it (see assembleProjectContextSnapshot): the project item (kind "project", id == projectId),
    // the selected-Run items (kind "run" / "manual_resolution", id == selectedRunId) and the workspace item
    // (kind "workspace", digest == workspaceFingerprint).
    bool mandatory {false};
};

// Items are in PRIORITY order (most important first, e.g. newest ledger row first); the loaders own that order.
struct ProjectContextSectionInput {
    ProjectContextCategory category {ProjectContextCategory::ProjectMetadata};
    std::vector<ProjectContextItem> items;
};

struct ProjectContextAssemblyInput {
    ProjectId projectId;
    std::optional<RunId> selectedRunId;
    std::string workspaceFingerprint;
    std::vector<ProjectContextSectionInput> sections;
    // Absolute path strings (in every spelling) that must never appear in the rendered text. The assembler
    // additionally rejects drive-letter, UNC / device, rooted (leading slash or backslash), home (~/) and
    // file: URI path shapes at a token boundary, whatever characters the segment uses.
    std::vector<std::string> forbiddenFragments;
};

struct ProjectContextSection {
    ProjectContextCategory category {ProjectContextCategory::ProjectMetadata};
    ProjectContextAuthority authority {ProjectContextAuthority::HumanCurated};
    std::string text;  // deterministic rendering: heading, then the included items, LF newlines
    std::vector<ProjectContextSourceRef> sources;  // included items only, in order
    std::uint64_t estimatedTokens {0};
    bool truncated {false};
    std::uint32_t includedItems {0};
    std::uint32_t omittedItems {0};
};

struct ProjectContextSectionBudgetReport {
    ProjectContextCategory category {ProjectContextCategory::ProjectMetadata};
    std::uint64_t limitTokens {0};
    std::uint64_t usedTokens {0};
    bool truncated {false};
    std::uint32_t omittedItems {0};
};

struct ProjectContextBudgetReport {
    std::string policyId {std::string(kProjectContextBudgetPolicyId)};
    std::uint64_t totalLimitTokens {0};
    std::uint64_t usedTokens {0};
    bool truncated {false};
    std::vector<ProjectContextSectionBudgetReport> sections;
};

struct ProjectContextSnapshot {
    std::string schemaVersion {std::string(kProjectContextSnapshotSchemaVersion)};
    std::string policyId {std::string(kProjectContextBudgetPolicyId)};
    ProjectId projectId;
    std::optional<RunId> selectedRunId;
    std::string workspaceFingerprint;
    std::vector<ProjectContextSection> sections;
    // All included sources in section order (derivable from `sections`; kept for read models).
    std::vector<ProjectContextSourceRef> sources;
    ProjectContextBudgetReport budget;
    // sha256 of the canonical serialization: schema, policy, project and selected-Run identity, workspace
    // fingerprint, budget and truncation facts, ordered source identities, authority/category and the exact
    // rendered text of every section. No assembly time, no absolute path.
    std::string fingerprint;
};

// Pure and deterministic: the same input always yields a byte-identical snapshot and fingerprint. Always
// uses the fixed v1 limits, so a returned snapshot is at most 24 000 estimated tokens and its policy id
// is truthful. Failure codes (all `project_context.*`): invalid_request, duplicate_section, invalid_item,
// duplicate_source, absolute_path, invalid_mandatory, missing_identity, mandatory_exceeds_budget,
// budget_unsatisfiable.
//
// Identity contract: the snapshot must state what it describes. It needs a ProjectMetadata section with a
// mandatory project item, a WorkspaceCatalog section with a mandatory workspace item (digest equal to the
// workspace fingerprint) and, exactly when a selected Run is given, a SelectedLineage section with a
// mandatory item for that Run. Mandatory items of any other role are rejected.
//
// Trimming: inside a section the non-mandatory items are kept as a PREFIX of the supplied order while the
// rendered section fits its own limit (an item that does not fit and everything after it is omitted). If
// the sections together still exceed the total, whole items are dropped from the tail of Project Memory
// first, then the Work Ledger, then the workspace catalog; the project metadata and the selected lineage
// are bounded only by their own limits and their mandatory items are never dropped.
[[nodiscard]] common::Result<ProjectContextSnapshot> assembleProjectContextSnapshot(
    const ProjectContextAssemblyInput& input);

// Tests and diagnostics only: the same assembly with explicit limits. The snapshot carries the v1 policy
// id only when `budget` equals the v1 limits; any other limit set is labelled "project_context_budget_custom"
// (in the snapshot and in the fingerprint), so a non-v1 context can never pass as v1.
inline constexpr std::string_view kProjectContextCustomBudgetPolicyId = "project_context_budget_custom";
[[nodiscard]] common::Result<ProjectContextSnapshot> assembleProjectContextSnapshotWithCustomBudget(
    const ProjectContextAssemblyInput& input, const ProjectContextBudget& budget);

// Every spelling (native, forward slash, backslash) of an absolute directory, for
// ProjectContextAssemblyInput::forbiddenFragments.
[[nodiscard]] std::vector<std::string> projectContextPathFragments(const std::filesystem::path& absolutePath);

// Shared path-leak guard for PC-1 loaders and the final assembler. `forbiddenFragments` normally
// contains every spelling of the project/workspace roots. This function does not validate general
// prose; it answers only whether the text contains an absolute-path shape or one of those exact roots.
[[nodiscard]] bool projectContextContainsForbiddenPath(
    std::string_view text,
    const std::vector<std::string>& forbiddenFragments) noexcept;

// Whether `text` is well-formed UTF-8 (no overlong forms, surrogates or truncated sequences). Loaders use it
// to decide that untrusted prose is unusable BEFORE the assembler would reject the whole snapshot for it.
[[nodiscard]] bool projectContextIsValidUtf8(std::string_view text) noexcept;

// The first 12 hex characters, for a compact UI label (the full value stays the identity).
[[nodiscard]] std::string shortProjectContextFingerprint(std::string_view fingerprint);

// The identity digest of a snapshot: sha256 of its canonical serialization, exactly what assembleProjectContextSnapshot()
// stores in `fingerprint`. It identifies content; it is not an authenticator. Fails only when a string in the snapshot is
// not well-formed UTF-8 (`project_context.invalid_text`).
[[nodiscard]] common::Result<std::string> computeProjectContextSnapshotFingerprint(const ProjectContextSnapshot& snapshot);

// Whether a snapshot handed over by a caller is what it claims to be: its `fingerprint` is the digest of the content it
// actually carries, its sections are in canonical order without duplicates, and every section's authority is the one
// its category fixes. Anything that is rendered, quoted, priced or sent as "the snapshot with fingerprint F" must pass
// this first, so the bytes that were reviewed are the bytes that are used. Failure codes (project_context.*):
// fingerprint_mismatch, invalid_structure, invalid_text.
[[nodiscard]] common::Result<void> validateProjectContextSnapshotIntegrity(const ProjectContextSnapshot& snapshot);

} // namespace ai::application
