#pragma once

#include "ai/application/NextRunPlan.hpp"
#include "ai/application/ProjectContextSnapshot.hpp"
#include "ai/common/Result.hpp"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace ai::application {

// PC-5A-3: deterministic, provider-free ADVISORY findings over a parsed `next-run-plan.v1` and the frozen project
// snapshot it was composed from. They are shown next to the other warnings of the PC-3D preview and never refuse,
// change or drop anything: a plan with advisories is exactly as valid as one without.
inline constexpr std::string_view kFollowUpAdvisorySingleUseSharedContract = "single_use_shared_contract";
inline constexpr std::string_view kFollowUpAdvisoryPathNotInCatalog = "path_not_in_catalog";
// File-name checks (6a (a) hardening, 5E canary: the composer wrote `Setion.qml` for `SettingsSection.qml` and lost two names).
inline constexpr std::string_view kFollowUpAdvisoryGoalNameNotInPlan = "goal_name_not_in_plan";
inline constexpr std::string_view kFollowUpAdvisoryTaskNameUnknown = "task_name_unknown";
// Staffing sanity (a suggested worker count above the number of files the draft names).
inline constexpr std::string_view kFollowUpAdvisoryStaffingExceedsNamedFiles = "staffing_exceeds_named_files";
// Run 249d9e18 (2026-10-19, Stage 12 of the dashboard project): a draft that named 51 files failed in PLANNING with
// planner.tool_call_budget_exceeded, because the Manifest Planner reads each named file with one tool call and has at most
// kAgentToolTurnLoopMaxTotalToolCalls (48) calls per session. A draft that names more than this many distinct files is flagged
// (half the cap: the Planner also re-reads and inspects the catalog). The composer prompt states the same number. The .cpp ties it
// to the Planner's cap with a static_assert, so the two cannot drift apart.
inline constexpr std::size_t kFollowUpDraftMaxNamedFiles = 24;
inline constexpr std::string_view kFollowUpAdvisoryDraftNamesManyFiles = "draft_names_many_files";
// At most this many advisories of each NAME code are produced (a flood says nothing more than the first few).
inline constexpr std::size_t kFollowUpMaxNameAdvisoriesPerCode = 12;

struct FollowUpPlanAdvisory {
    std::string code;
    std::string message;
    std::string contractId;
    // single_use_shared_contract: the one draft that lists the contract. path_not_in_catalog: the draft that owns
    // the contract, or empty for a shared contract. task_name_unknown / staffing_exceeds_named_files: the draft (task_name_unknown
    // names a shared contract through contractId instead and leaves this empty). goal_name_not_in_plan: empty.
    std::string draftKey;
    // path_not_in_catalog: the structured `appliesToPaths` entry, exactly as the plan carries it. goal_name_not_in_plan and
    // task_name_unknown: the file name as written.
    std::string path;
    // path_not_in_catalog only: the visible catalog was cut, so the entry may name a file that exists but was not
    // shown. Advisory either way.
    bool catalogTruncated {false};
};

// `single_use_shared_contract`: a plan.sharedContracts entry that exactly ONE draft lists (a contract nobody lists
// is the parser's own warning; one that two drafts list is what a shared contract is for).
//
// `path_not_in_catalog`: a STRUCTURED `appliesToPaths` entry of a shared contract or of a draft's own contract that
// covers no `workspace_file` source of the snapshot's workspace catalog. "Covers" is the PC-2 scope rule itself
// (runContractScopeCoversPath: equal or below, separators and ASCII case normalized), not string equality. Task
// prose is never parsed for paths. A catalog that was cut can only make this advisory fire for a file that exists
// but was not shown; it is never a refusal.
//
// Order: the shared contracts in plan order (a contract's single-use advisory, then its path advisories in scope
// order), then each draft's own contracts in draft and contract order. A shared contract is reported once, not once
// per draft that lists it. It has no failure path: an inconsistent plan (a `sharedContractCount` beyond its contract
// list, say) is reported as far as it can be read.
//
// File-name checks run only when `goal` is not empty (heuristic, advisory, never a refusal; Confirm is not gated by them).
// A "file name" is a token of path-safe characters whose last dot-extension is in a closed list (or a path ending so), compared by
// its base name ignoring ASCII case. `shownContext` is the exact user content the model was sent (it contains the goal, the
// snapshot, the brief and the excerpts), so a name the model was SHOWN is never reported as unknown.
//  - goal_name_not_in_plan: a name of the goal that no title, task, contract text or scope of the plan names.
//  - task_name_unknown: a name in a draft's title/task/own contract text (or in a shared contract's text) that is in none of the
//    goal, the shown context and the visible catalog (a garbled or invented name; a new helper file is reported too, on purpose).
//  - draft_names_many_files: a draft whose title, task and own contracts name more than kFollowUpDraftMaxNamedFiles distinct files
//    (counted ignoring ASCII case). Independent of the goal; draftKey = the draft.
//  - staffing_exceeds_named_files: a draft's suggested worker count is above the number of distinct names its task and contracts
//    name (only when it names at least one). Independent of the goal.
// Order: the existing advisories first, then goal_name_not_in_plan, then per draft task_name_unknown and staffing, then the
// shared contracts' task_name_unknown.
[[nodiscard]] std::vector<FollowUpPlanAdvisory> computeFollowUpPlanAdvisories(
    const NextRunPlan& plan,
    const ProjectContextSnapshot& snapshot,
    std::string_view goal = {},
    std::string_view shownContext = {});

// The file names a text contains, as written (leading "./" and "/" dropped), unique ignoring ASCII case, first-seen order. The
// rule of the checks above, exported so the tests pin it.
[[nodiscard]] std::vector<std::string> extractFollowUpFileNames(std::string_view text);

// The stored form (`FollowUpPlanAnswer::advisoriesJson`): a JSON array of objects with exactly the six fields of the struct. The
// writer keeps the longest PREFIX that fits `maxBytes` (an advisory is never half-written and never reordered), so a plan with an
// unusual number of findings can never make the answer unstorable; the findings are advisory either way.
// The reader is strict: an array of objects with exactly those fields and types, else `follow_up.invalid_advisories`.
[[nodiscard]] std::string serializeFollowUpPlanAdvisories(
    const std::vector<FollowUpPlanAdvisory>& advisories, std::size_t maxBytes);
[[nodiscard]] common::Result<std::vector<FollowUpPlanAdvisory>> parseFollowUpPlanAdvisories(std::string_view stored);

} // namespace ai::application
