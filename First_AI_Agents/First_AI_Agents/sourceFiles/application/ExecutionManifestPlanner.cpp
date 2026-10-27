#include "ai/application/ExecutionManifestPlanner.hpp"
#include "ai/application/CostCalculation.hpp"
#include "ai/application/workspace/FilesystemPathUtils.hpp"
#include "ai/application/RunWorkspaceScopeService.hpp"

#include "ai/application/ExecutionManifestDraftCompiler.hpp"
#include "ai/application/ExecutionManifestExecutionCapacity.hpp"
#include "ai/application/ExecutionManifestDraftParser.hpp"
#include "ai/application/ExecutionManifestPlannerTurnBudget.hpp"
#include "ai/application/ExecutionManifestRepairControls.hpp"
#include "ai/application/IdGenerator.hpp"
#include "ai/application/ManifestMandatoryVerificationPolicy.hpp"
#include "ai/application/ManifestRouting.hpp"
#include "ai/application/ManifestVerificationProfileBinder.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace ai::application {

namespace {

constexpr std::size_t kMaximumProjectionRevisionBytes = 512;
constexpr std::size_t kMaximumProjectionPaths = 512;
constexpr std::size_t kMaximumProjectionPathBytes = 4096;
constexpr std::size_t kMaximumProjectionDiffBytes = 64ULL * 1024;

const char* const kPlannerSystemPrompt = R"PROMPT(
You are the inspect-only Lead Architect. Inspect the workspace with the
advertised read-only tools and produce a bounded execution graph. You cannot
modify files and you must never call or emit propose_change_set. Raw .git
access is denied. Repository state, when available, is supplied only through
a trusted typed projection in the user message.

Your terminal action is exactly one call to one advertised terminal submission
tool - never alongside another tool call in the same turn, and never more than
once. Before choosing it, perform a
completion preflight against the CURRENT Run task: classify every requested
acceptance criterion as already satisfied or still requiring concrete
downstream work. Do not silently expand the task to a later catalog Stage.

Call submit_execution_manifest_draft when work remains. Its arguments must
follow execution-manifest-draft.v1 exactly and always contain exactly these
seven top-level fields, none ever omitted: schemaVersion (the literal
execution-manifest-draft.v1), coordinationTasks, workTasks,
requestedMaxParallelism, requestedTokenBudget, requestedCostMicros,
bootstrapDeclaration. The two budget fields are never left out even when
there is no budget - set them to the JSON literal null instead of omitting
the key. bootstrapDeclaration is the JSON literal null unless you are
declaring a same-Run verification-environment bootstrap (see below) - never
inferred from any other field. Do not include runId, policy, provider/model
choices, timestamps, prose, markdown fences, or extra fields: those values
belong to the Backend trust boundary.

There are two distinct task shapes, in two separate arrays - never mix
them. A coordinationTasks entry is a pure dependency/coordination anchor
with no work of its own: it always contains exactly these four fields,
none ever omitted: id, objective, role, dependencies. It structurally
cannot own a path, read anything, or carry acceptance criteria - there is
no field for any of that, so there is nothing to keep empty. Use it only
when you genuinely need an empty ordering node; most Manifests need none
at all. A workTasks entry always contains exactly these thirteen fields,
none ever omitted: id, objective, role, dependencies, writePaths,
baselineRequiredReadPaths, proposedDependencyReadPaths, referencePaths,
excludedPaths, deferredPaths, acceptanceCriteria, acceptanceContract,
contextReferences, requestedCapability (inspect or propose only - never
"none" here). All five path-role array fields (writePaths,
baselineRequiredReadPaths, proposedDependencyReadPaths, referencePaths,
plus excludedPaths/deferredPaths below) must be present. writePaths,
baselineRequiredReadPaths, proposedDependencyReadPaths, and referencePaths
contain only workspace-relative path strings. excludedPaths and
deferredPaths contain only objects with exactly two string fields: path
and reason; reason must be nonblank and explain the deliberate exclusion
or deferral. Use [] when any path role has no entries instead of omitting
its key.
acceptanceContract on a workTasks entry is always a real
execution-acceptance.v1 object - never the JSON literal null and never
omitted; there is no representable empty state for it, because a task with
no real acceptance belongs in coordinationTasks instead, which never has
this field at all. acceptanceContract always contains exactly these
two fields, none ever omitted: schemaVersion (the literal
execution-acceptance.v1) and requirements (an array; the field itself is
always present even though each item inside it carries its own fields).
Every requirement object always contains exactly these ten fields, none
ever omitted: id (stable), description (nonblank), requiredOutputs (an
array of workspace-relative paths - may be an empty array but the field
itself is always present), verificationKind (a Backend-known kind:
inspect_paths, cmake_build, or qml_lint),
verificationProfileId, verificationBuildProfileId, verificationTargetId
(each either a string id taken verbatim from the trusted verification
environment catalog below, or the JSON literal null), verificationArguments
(an array - use [] when this kind needs no directly-submitted arguments;
never omit this field just because it would be empty), required (a
boolean), and verificationBootstrapPending (a boolean - true only for a
cmake_build/qml_lint requirement legitimately deferred pending the
bootstrapDeclaration described below; false in every other case). Every
verificationArguments item, when the array is non-empty,
always contains exactly these two fields, none ever omitted: name and
value, both strings. A task, an acceptanceContract object, a requirement
object, or a verificationArguments item that omits any one of its own
fields rejects the entire submission - do not shorten acceptanceContract to
only requirements, and do not shorten a requirement to only
id/requiredOutputs/verificationKind.

You never author a raw CMake configure preset name, build preset name,
build directory, executable name, command-line argument, toolchain path, or
shell command anywhere in this submission. Those are exclusively
Backend-resolved from the ids you select in verificationProfileId/
verificationBuildProfileId/verificationTargetId, using ONLY the ids
literally advertised in the trusted verification environment catalog
supplied separately in this message - never an id you infer, guess, or
recombine from catalog text. The exact selection contract per kind, with no
extra names or ids permitted beyond what is stated: inspect_paths needs no
profile/build-profile/target selection (all three null) and an empty
verificationArguments []. cmake_build requires a real catalog
verificationProfileId and its linked verificationBuildProfileId
(verificationTargetId stays null); verificationArguments stays [] - the
Backend resolves preset and buildPreset itself. qml_lint requires all three:
verificationProfileId, verificationBuildProfileId, and a
verificationTargetId taken from that build profile's own confirmed
allowedTargets list; verificationArguments stays [] - the Backend resolves
buildDirectory and target itself. Only select a profile/build profile whose
buildPresetsConfirmed and (for qml_lint) allowedTargetsConfirmed flags are
true in the catalog; an unconfirmed one is not currently usable. If the
catalog offers no usable profile at all (the trusted catalog says so
explicitly), this Run's verification environment is SetupRequired, and your
ONLY two safe terminal actions for this call are: (1) submit_execution_manifest_draft
with a valid same-Run bootstrap declared, or (2) call
submit_verification_action_required. In this state, submit_no_changes_needed
is never accepted, and an ordinary submit_execution_manifest_draft that omits
bootstrapDeclaration is never accepted either - not even one that only
contains inspect_paths requirements and would otherwise need no environment
verification at all. Never invent a profile id to work around this either
way. To take option (1), only when this Run is authorized to create or
modify the missing setup: set the top-level bootstrapDeclaration to an
object with bootstrapTaskId (the single workTasks entry that owns the setup
file(s) via its own writePaths - the Backend classifies each as
existing/new from the baseline, never split this yourself) and setupPaths
(exactly
CMakePresets.json and/or CMakeUserPresets.json at the repository root -
never a subdirectory, never any other filename, and never claimed by any
other task in any path-role list). Then, for exactly the cmake_build/
qml_lint requirements that depend on the environment this bootstrap
creates, set verificationBootstrapPending true and leave
verificationProfileId, verificationBuildProfileId, and verificationTargetId
all null and verificationArguments empty - the Backend resolves them later
from your bootstrap's own proposed content, never from anything you claim
here. Every task with such a deferred requirement must list bootstrapTaskId
in its own dependencies so it runs after the bootstrap. Never set
bootstrapDeclaration merely because the project happens to contain CMake
files, and never set verificationBootstrapPending without a matching
bootstrapDeclaration. To take option (2) instead, when this Run is not
authorized or able to safely create the missing setup, call
submit_verification_action_required with schemaVersion
(verification-action-required.v1), a nonblank summary, a nonblank reason
explaining why no safe bootstrap could be formed, and recommendedNextStep
(a string or the JSON literal null) - never a raw path, command, provider
payload, preset JSON, or secret in any of these fields. This call must be
the only tool call in its turn, exactly like the other two terminal
submissions. There is no cmake_target_consistency kind: the Backend has no
trusted catalog of admissible definition/registration/target triples yet, so
this kind is never accepted from a submission - it exists only as an
internally-verified kind for legacy Manifests that already predate this
restriction. For ordinary CMake target/registration consistency, use
cmake_build (and, if the target is a QML lint target, qml_lint) against a
real catalog profile instead; never invent a source-file substring check of
your own, and never substitute a generated build-system target name (for
example one ending in _qmllint) into any kind of literal consistency check -
generated targets are not required to appear verbatim in your source files.
A target-only cmake_build is invalid; a
cmake_build/qml_lint verificationArguments that is not empty is rejected
outright as a forbidden raw environment override. If the CURRENT task
requires both a configure/build check and QML lint, represent them as
separate cmake_build and qml_lint requirements. Because verification uses a
source-only candidate and never copies an existing build tree, every
Manifest that contains qml_lint must also contain a required cmake_build
requirement; the Backend runs that fresh configure/build before lint and
fails closed if it does not pass. cmake_build, qml_lint, and
cmake_target_consistency are command-backed: they are evaluated exactly
once, automatically, across the whole Manifest by the Backend's own
combined verification after every propose-capability task's proposal is
in place - never by an individual worker session, and never by an
inspect-capability task. An inspect-capability task's worker session only
reads files and self-reports evidence through submit_task_verification;
it cannot run a build or lint command, so its acceptanceContract may only
contain inspect_paths requirements. Do not attach cmake_build, qml_lint,
or cmake_target_consistency to an inspect-capability task's
acceptanceContract even when that task's role is "verify the build/lint
result" - model that verification role as a propose-capability task's own
requirement instead (or rely on the Run's ordinary combined verification
with no dedicated task at all).
Free-form acceptanceCriteria remain context; they are not automatically
treated as verified evidence. For an inspect_paths requirement, every
requiredOutputs entry must be a real workspace-relative file path that will
actually exist once this task's changes are applied - the Backend checks
each one with a literal filesystem existence check, so a descriptive phrase
(for example "build verification notes" or "tests pass") is never valid
there and always fails closed. Every requiredOutputs path must also appear in
that same task's writePaths, baselineRequiredReadPaths, or
proposedDependencyReadPaths; it may
not be merely a reference, excluded, or deferred path. Never invent an extra
requirement merely to
represent "the build works" or "lint is clean" as inspect_paths: use
cmake_build or qml_lint with a real catalog profile selection for that, or simply
do not add a requirement for it and rely on the task's acceptanceCriteria
plus the file-existence requirements you already created for the files you
are actually producing. The graph must be acyclic and fit the stated
Backend limits. A coordinationTasks entry is only an empty dependency/
coordination anchor: it owns no path and claims no acceptance (see above -
this is structural, not a convention to remember).

writePaths are exclusive file-exact write/working boundaries, not a list of
everything you inspected, and not split by you into existing-versus-new - the
Backend alone classifies each entry against the frozen baseline. Every value
must be one clean workspace-relative
file using forward slashes, for example src/app/Main.cpp or tests/unit/Test.cpp.
Never use an empty value, repository root, '.', './', a leading or trailing
slash, an absolute/drive path, '..', a backslash, or glob characters such as
'*' or '?'. Paths written by different tasks must be disjoint: equality and
ancestor/descendant pairs such as src and src/app both overlap. Do not create
broad repository-audit ownership or assign the same integration/report path
to multiple tasks.

Within one task, every normalized path belongs to exactly one of the six
path-role lists (writePaths, baselineRequiredReadPaths,
proposedDependencyReadPaths, referencePaths, excludedPaths, deferredPaths);
never repeat a path across roles. Any file this task writes - whether it
already exists or is brand new - belongs only in writePaths. The worker is
already allowed to read its own writePaths, so never duplicate a written path
in either read list or referencePaths merely because it must be inspected
before editing.

Read paths split into two lists by WHY you need them, never by filename,
extension, or guesswork - state the real reason explicitly:
baselineRequiredReadPaths is for a path whose CURRENT, already-existing
content this task needs, regardless of whether some other task's writePaths
also includes that same path to change it LATER in this same Manifest; this
never creates a dependency on that other task, because you are not waiting
for its result. proposedDependencyReadPaths is for a path this task
genuinely cannot proceed without another specific task in THIS submission
first producing - that other task's writePaths must contain the exact same
path, or the submission is rejected; this DOES create a real ordering
dependency, and the task must actually be able to run only after that
producer. When in doubt whether you need the baseline snapshot or another
task's fresh result, ask which one your task's own work actually depends on
- most reads of a shared, pre-existing project file (documentation, a design
token file, a build/module-registration file) are baselineRequiredReadPaths,
not proposedDependencyReadPaths, even when some other task also happens to
write that same file. referencePaths are optional read-only context with no
same-attempt inspection requirement at all. excludedPaths and deferredPaths
are inactive decisions and must not also appear in any active role.

excludedPaths is not a synonym for "read-only" - it means this task has no
legitimate reason to read or write the path at all, and must give a genuine
reason for that exclusion. A file must never be excluded merely because it
should not be MODIFIED; "must not edit" alone always means
baselineRequiredReadPaths or proposedDependencyReadPaths (whichever
actually applies - see above; or referencePaths for optional context),
never excludedPaths. This matters
most for files that define a contract other written files actually consume -
theme/design-token singletons, other singleton or component APIs a task's
own QML/code imports, and build or module-registration files (for example
qml/CMakeLists.txt or a module's qmldir) that govern how written files resolve
their own imports. If a task's own files reference declarations from such a
file, that file belongs in baselineRequiredReadPaths (or referencePaths),
even though
the task must never write to it - excluding it instead can make a later
verification failure about that exact contract unrepairable, because the
worker then has no legitimate way to inspect the authoritative declaration
before correcting a wrong reference to it.

Real-Run incident (Runs 37640091-.../207df296-.../e8275bde-.../397c0ffb-.../
2d2cf273-.../3ffb52df-.../a8afe6a0-..., 2026-09-14, real Stage 2B on OpenAI):
a standing project-
context or documentation file that already exists in the baseline (for
example QML_PROJECT_CONTEXT.md or CODEX_CONTEXT.md) is never owned or
planned-create by ANY task merely because another task also needs to read
it. If this Run's own work does not substantively rewrite that file's
content as one of its real deliverables, it has no producer in this
Manifest at all - list it directly in baselineRequiredReadPaths (never
proposedDependencyReadPaths, since no task actually produces it) (or
referencePaths
for optional context) of EVERY task that needs it, exactly like any other
pre-existing file a task inspects but does not modify. Inventing a
writePaths claim on such a file for one task, purely to
give another task's read "something to depend on," creates a
producer-consumer edge that does not reflect this Run's actual work and is
the single most common real cause of an otherwise-inexplicable dependency
cycle between two tasks whose real jobs do not actually depend on each
other - see manifest.dependency_cycle below. This is exactly what
baselineRequiredReadPaths exists for: declare the read there and the
Backend never derives a false dependency from it, even when the same path
is genuinely also another task's writePaths entry for later work.

The Manifest is execution work, not a way to continue this planning session.
Do not create tasks whose only purpose is repository audit, inspection,
verification, or reporting merely to avoid a no-op. Perform preflight reads
now. If those reads show that no requested acceptance criterion needs a file
change or other independently executed task, submit no changes only when that
terminal tool is advertised by the trusted completion policy below.
When the trusted policy says this is an implementation Run that requires a
Manifest, the graph must contain at least one propose-capability workTasks
entry with at least one exact writePaths file. An inspect-only graph is
not an implementation result. A user instruction such as "do not apply
automatically" never means "do not propose changes": workers only prepare
reviewable proposals and the separate human Apply action remains unchanged.

Call submit_no_changes_needed only when it is advertised and, after inspecting the workspace, the
current Run task's acceptance criteria are already satisfied: never invent a
write task merely to make the graph non-empty. This call is only valid after
at least one successful inspection tool call in THIS session - a prior
session's inspections (for example before a repair) never count, so a fresh
repair session must inspect again on its own before it may claim no changes
are needed. Its arguments always contain exactly these four fields, none
ever omitted: schemaVersion (the literal execution-manifest-no-changes.v1),
summary (non-empty), evidence, and recommendedNextStep. evidence is a
bounded, honest list; each entry must name the criterion it addresses, the
exact workspace-relative path of a tool call you actually issued and that
succeeded in this same session, and what you actually observed there - an
evidence path that was never inspected in this session is rejected even if
some other path was. recommendedNextStep is either a short suggestion
string for a human reviewer or the JSON literal null when you have no
suggestion - never omit the field itself, and never use it to silently
continue into the next catalog Stage or any other unrequested work.
)PROMPT";

std::string finishReasonText(providers::ProviderAgentFinishReason reason)
{
    switch (reason) {
    case providers::ProviderAgentFinishReason::Stop: return "stop";
    case providers::ProviderAgentFinishReason::ToolUse: return "tool_use";
    case providers::ProviderAgentFinishReason::MaxTurns: return "max_turns";
    case providers::ProviderAgentFinishReason::MaxOutputTokens: return "max_output_tokens";
    case providers::ProviderAgentFinishReason::Error: return "error";
    case providers::ProviderAgentFinishReason::StopRequested: return "stop_requested";
    }
    return "error";
}

common::Result<void> validateProjection(
    const std::optional<RepositoryInspectionProjection>& projection)
{
    if (!projection.has_value()) return common::Result<void>::ok();
    if (projection->revision.size() > kMaximumProjectionRevisionBytes
        || projection->changedPaths.size() > kMaximumProjectionPaths
        || projection->diffSummary.size() > kMaximumProjectionDiffBytes) {
        return common::Result<void>::failure(
            {"planner.repository_projection_too_large",
             "Repository inspection projection exceeds its configured limit"});
    }
    for (const auto& path : projection->changedPaths) {
        if (path.size() > kMaximumProjectionPathBytes) {
            return common::Result<void>::failure(
                {"planner.repository_projection_too_large",
                 "Repository inspection projection contains an oversized path"});
        }
    }
    return common::Result<void>::ok();
}

std::string appendProjection(
    std::string userContent,
    const std::optional<RepositoryInspectionProjection>& projection)
{
    if (!projection.has_value()) return userContent;
    const nlohmann::json document {
        {"revision", projection->revision},
        {"changedPaths", projection->changedPaths},
        {"diffSummary", projection->diffSummary}
    };
    userContent += "\n\n# Trusted repository inspection projection\n";
    userContent += document.dump();
    return userContent;
}

struct AttemptResult {
    bool submitted {false};
    bool rejected {false};
    bool cancelled {false};
    std::string submission;
    std::string errorCode;
    std::string errorMessage;
    // Appended last so every pre-existing 6-element aggregate init below
    // keeps compiling unchanged, defaulting to false/0/empty via the
    // in-class initializers; only the terminal-submission branch sets
    // these explicitly - every other return path is irrelevant to
    // no-changes-needed evaluation, which is the only consumer of them.
    bool noChangesSubmission {false};
    // Successful inspection tool calls made by THIS attempt's session only
    // (never carried over from a prior primary/repair attempt).
    std::size_t attemptSuccessfulInspectionCalls {0};
    // Normalized (see normalizeComparablePath) paths this attempt's session
    // actually inspected successfully - the trusted set a no-changes-needed
    // evidence[] path is checked against.
    std::unordered_set<std::string> attemptInspectedPaths;
    // AG-18.3 hardening (blocker 2): true only when the terminal call was
    // submit_verification_action_required. Appended last for the same
    // positional-aggregate-init compatibility reason as noChangesSubmission
    // above.
    bool actionRequiredSubmission {false};
};

bool isBlank(const std::string& value)
{
    return value.empty()
        || std::all_of(value.begin(), value.end(), [](unsigned char character) {
               return std::isspace(character) != 0;
           });
}

// Case/separator-insensitive comparable form for a workspace-relative path.
// Both sides of the no-changes-needed evidence-path check (the model's
// claimed evidence path and the actual argument of a successful inspect
// tool call) are normalized through this same function before comparison.
// Safety (traversal, absolute paths, etc.) is validated separately by
// WorkspaceReadPolicyValidator; this only makes textual comparison robust
// to backslash-vs-slash and case differences.
std::string normalizeComparablePath(const std::string& value)
{
    std::string normalized = value;
    std::replace(normalized.begin(), normalized.end(), '\\', '/');
    std::transform(normalized.begin(), normalized.end(), normalized.begin(),
        [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
    return normalized;
}

// Provider schemas can enforce each list's shape, but cannot express
// cross-array uniqueness. Models also naturally classify an existing file
// they will modify as both owned_write and required_read. At the Planner
// boundary, collapse only those redundant ACTIVE classifications whose
// precedence cannot enlarge authority:
//
//   owned_write/planned_create > required_read > reference_only
//
// This pass only removes entries and preserves first-seen order. It never
// resolves owned_write vs planned_create, never touches excluded/deferred
// decisions, and therefore leaves every contradictory or authorization-
// relevant combination for ExecutionManifestValidator to reject fail-closed.
void canonicalizeRedundantActiveScopeRoles(ExecutionManifest& manifest)
{
    const auto deduplicate = [](std::vector<std::filesystem::path>& paths) {
        std::unordered_set<std::string> seen;
        std::erase_if(paths, [&](const std::filesystem::path& path) {
            return !seen.insert(normalizeComparablePath(detail::genericUtf8(path))).second;
        });
    };
    const auto keysFor = [](const std::vector<std::filesystem::path>& paths) {
        std::unordered_set<std::string> keys;
        keys.reserve(paths.size());
        for (const auto& path : paths)
            keys.insert(normalizeComparablePath(detail::genericUtf8(path)));
        return keys;
    };
    const auto removeKeys = [](std::vector<std::filesystem::path>& paths,
                               const std::unordered_set<std::string>& keys) {
        std::erase_if(paths, [&](const std::filesystem::path& path) {
            return keys.contains(normalizeComparablePath(detail::genericUtf8(path)));
        });
    };

    for (auto& task : manifest.tasks) {
        deduplicate(task.ownedPaths);
        deduplicate(task.plannedCreatePaths);
        deduplicate(task.requiredReadPaths);
        deduplicate(task.referencePaths);
        // MW-P3A package 2: baselineOnlyReadPaths is a Backend-only marker
        // on a subset of requiredReadPaths (see ExecutionManifestTask's own
        // field comment) - it must be kept in exact lockstep with whatever
        // this pass does to requiredReadPaths itself, or the validator's
        // own subset invariant (manifest.baseline_only_read_path_not_required)
        // would spuriously fire on an entry this pass legitimately removed
        // from requiredReadPaths for an unrelated reason (a redundant
        // duplicate, or a path also claimed as a write in this same task).
        deduplicate(task.baselineOnlyReadPaths);

        auto writeKeys = keysFor(task.ownedPaths);
        const auto plannedCreateKeys = keysFor(task.plannedCreatePaths);
        writeKeys.insert(plannedCreateKeys.begin(), plannedCreateKeys.end());

        removeKeys(task.requiredReadPaths, writeKeys);
        removeKeys(task.baselineOnlyReadPaths, writeKeys);
        removeKeys(task.referencePaths, writeKeys);
        const auto requiredReadKeys = keysFor(task.requiredReadPaths);
        removeKeys(task.referencePaths, requiredReadKeys);
        // Any baselineOnlyReadPaths entry no longer present in
        // requiredReadPaths at all (removed above by deduplicate/removeKeys)
        // can never be a valid marker any more - drop it too, for the same
        // subset-invariant reason.
        std::erase_if(task.baselineOnlyReadPaths, [&](const std::filesystem::path& path) {
            return !requiredReadKeys.contains(normalizeComparablePath(detail::genericUtf8(path)));
        });
    }
}

// The highest requestedMaxParallelism the draft schema lets a provider submit.
constexpr std::uint32_t kDraftSchemaMaximumParallelism = 32;

// requestedMaxParallelism is a request, not a capability: the executor never runs more tasks at
// once than the Run's trusted ceiling, and the validator would otherwise reject a larger request
// and spend a paid repair session on a one-number fix. The Planner is never told the ceiling
// (Stage 9B Run cbe83247: the request exceeded it in 4 of 6 roster Runs, three recovered through
// a repair and this one regressed into planner.repair_exhausted). Lower such a request to the
// ceiling and report it. This can only reduce concurrency, never grant any authority. A zero
// request, a value above the draft schema's own maximum and an invalid trusted policy are left
// untouched for ExecutionManifestValidator to reject fail-closed.
std::optional<std::string> clampRequestedParallelismToCeiling(
    ExecutionManifest& manifest, const ExecutionManifestPolicy& policy)
{
    const std::uint32_t requested = manifest.requestedMaxParallelism;
    if (policy.maxParallelism == 0 || policy.maxParallelism > kDraftSchemaMaximumParallelism
        || requested <= policy.maxParallelism || requested > kDraftSchemaMaximumParallelism) {
        return std::nullopt;
    }
    manifest.requestedMaxParallelism = policy.maxParallelism;
    return "requestedMaxParallelism " + std::to_string(requested)
        + " exceeded the Run's parallelism limit and was lowered to "
        + std::to_string(policy.maxParallelism);
}

// Verification criteria are keyed by requirement id across the WHOLE Manifest: the proposal-set
// verifier flattens every task's requirements into one criteria list and rejects a repeated id with
// verification.invalid_criterion (nothing is evaluated), and the attribution service refuses a
// Manifest with a duplicate requirement identity. The validator only checks uniqueness inside ONE
// task, and the Planner is free to give independent tasks the same generic ids (Stage 10A-Repair
// Run 8e22e38a: both tasks declared build-local-debug and lint-dashboard-content; planning accepted
// it, both workers were paid and verification then failed). The first task, in Manifest order,
// keeps an id; a later task that repeats it gets "<id>-<taskId>" (with a counter if that exists
// too). An id repeated inside one task is left for the validator to reject. This is a pure rename:
// kind, arguments, outputs and the required flag are untouched, nothing is granted, and every
// rename is reported in the audit.
std::vector<std::string> canonicalizeCrossTaskRequirementIds(ExecutionManifest& manifest)
{
    std::vector<std::string> audit;
    std::unordered_set<std::string> taken;
    for (const auto& task : manifest.tasks) {
        if (!task.acceptanceContract.has_value()) continue;
        for (const auto& requirement : task.acceptanceContract->requirements)
            taken.insert(requirement.id);
    }
    std::unordered_map<std::string, std::string> ownerById;
    for (auto& task : manifest.tasks) {
        if (!task.acceptanceContract.has_value()) continue;
        for (auto& requirement : task.acceptanceContract->requirements) {
            const auto [position, inserted] = ownerById.emplace(requirement.id, task.id);
            if (inserted || position->second == task.id) continue;
            const std::string firstOwner = position->second;
            std::string candidate;
            for (std::size_t attempt = 1;; ++attempt) {
                candidate = requirement.id + "-" + task.id
                    + (attempt == 1 ? std::string() : "-" + std::to_string(attempt));
                if (!taken.contains(candidate)) break;
            }
            if (candidate.size() > kMaximumAcceptanceStringBytes) continue;
            audit.push_back("requirement id '" + requirement.id + "' of task '" + task.id
                + "' repeated the id of task '" + firstOwner + "' and was renamed to '"
                + candidate + "'");
            taken.insert(candidate);
            ownerById.emplace(candidate, task.id);
            requirement.id = std::move(candidate);
        }
    }
    return audit;
}

// A required-read of another task's produced file implies a scheduler edge;
// omitting that edge can never make the graph safer or more parallel because
// the consumer cannot inspect the producer's candidate output before the
// producer finishes.  JSON Schema cannot express this cross-task relation and
// real provider runs repeatedly reproduced the same missing edge even after a
// precise bounded-repair finding.  Complete only this mechanically provable
// edge at the Planner trust boundary.  The ordinary manifest validator still
// rejects unknown dependencies and cycles afterwards, so this pass can only
// restrict execution order - it never grants read/write authority or repairs
// an ambiguous ownership graph.
void canonicalizeRequiredReadDependencies(ExecutionManifest& manifest)
{
    std::unordered_map<std::string, std::optional<std::string>> producerByPath;
    const auto recordProducer = [&](const std::filesystem::path& path,
                                    const std::string& taskId) {
        const auto key = normalizeComparablePath(detail::genericUtf8(path));
        const auto [it, inserted] = producerByPath.emplace(key, taskId);
        if (!inserted && it->second.has_value() && *it->second != taskId) {
            // Multiple writers remain an authoritative validator error.  Do
            // not guess which producer a consumer should depend on.
            it->second.reset();
        }
    };
    for (const auto& task : manifest.tasks) {
        for (const auto& path : task.ownedPaths) recordProducer(path, task.id);
        for (const auto& path : task.plannedCreatePaths) recordProducer(path, task.id);
    }

    for (auto& task : manifest.tasks) {
        // MW-P3A package 2 (Run a8afe6a0-7eb1-4c05-aaf3-46a1c5c90b6b): a
        // baselineOnlyReadPaths entry must never auto-inject a declared
        // dependency either, for exactly the same reason
        // deriveManifestDependencyGraph never derives an edge from one and
        // RunWorkspaceScopeService's missing_scope_dependency check never
        // requires one - it is a read of the frozen baseline, not of
        // another task's proposed output, even when the same path also
        // happens to be another task's write target. Silently completing
        // this "edge" here would recreate the exact same false-dependency
        // defect a third way. Always empty for a legacy-parsed manifest, so
        // this changes nothing for that pathway.
        std::unordered_set<std::string> baselineOnly;
        baselineOnly.reserve(task.baselineOnlyReadPaths.size());
        for (const auto& path : task.baselineOnlyReadPaths) {
            baselineOnly.insert(normalizeComparablePath(detail::genericUtf8(path)));
        }
        for (const auto& path : task.requiredReadPaths) {
            const auto key = normalizeComparablePath(detail::genericUtf8(path));
            if (baselineOnly.contains(key)) continue;
            const auto producer = producerByPath.find(key);
            if (producer == producerByPath.end() || !producer->second.has_value()
                || *producer->second == task.id) {
                continue;
            }
            if (std::find(task.dependencies.begin(), task.dependencies.end(),
                    *producer->second) == task.dependencies.end()) {
                task.dependencies.push_back(*producer->second);
            }
        }
    }
}

// JSON Schema can validate each requiredOutputs entry as a safe relative path,
// but it cannot relate that string to the task's frozen file allocation. A
// production Run demonstrated the gap with requiredOutputs=["inspection_notes"]:
// syntactically path-like, but neither an existing/read file nor a file the
// task could create. The inspect worker could therefore never produce a valid
// same-attempt evidence path. Keep this Planner-boundary check separate from
// the reusable acceptance-contract parser: it needs the enclosing task scope.
std::vector<ExecutionManifestFinding> plannerSemanticFindings(
    const ExecutionManifest& manifest,
    bool allowEvidenceBackedNoChanges,
    WorkspaceCapability trustedMaximumCapability)
{
    std::vector<ExecutionManifestFinding> findings;

    // PlanningCompletionPolicy::RequireManifest is the safe implementation
    // default. When proposing is permitted, accepting a graph made entirely of
    // audit/inspect nodes turns known missing work into a false completion with
    // no Change Proposal. Profiles whose trusted ceiling is Inspect remain able
    // to run genuine audit-only Manifests.
    if (!allowEvidenceBackedNoChanges
        && trustedMaximumCapability == WorkspaceCapability::Propose
        && !manifest.tasks.empty()) {
        const bool hasExecutableProposalTask = std::any_of(
            manifest.tasks.begin(), manifest.tasks.end(), [](const auto& task) {
                return task.requestedCapability == WorkspaceCapability::Propose
                    && (!task.ownedPaths.empty() || !task.plannedCreatePaths.empty());
            });
        if (!hasExecutableProposalTask) {
            findings.push_back({
                "planner.non_executable_manifest",
                "An implementation Run requires at least one propose task with an exact write path",
                std::nullopt});
        }
    }

    for (const auto& task : manifest.tasks) {
        if (!task.acceptanceContract.has_value()) continue;
        std::unordered_set<std::string> requiredOutputScope;
        const auto addPaths = [&](const std::vector<std::filesystem::path>& paths) {
            for (const auto& path : paths)
                requiredOutputScope.insert(
                    normalizeComparablePath(detail::genericUtf8(path)));
        };
        addPaths(task.ownedPaths);
        addPaths(task.plannedCreatePaths);
        addPaths(task.requiredReadPaths);

        for (const auto& requirement : task.acceptanceContract->requirements) {
            for (const auto& output : requirement.requiredOutputs) {
                if (!requiredOutputScope.contains(normalizeComparablePath(output))) {
                    findings.push_back({
                        "manifest.acceptance_output_outside_task_scope",
                        "Every required acceptance output must be an owned, planned-create, or required-read file of its task",
                        task.id});
                    break;
                }
            }
        }
    }
    return findings;
}

// Every tool in ToolRegistry::createDefaultReadOnly() takes a "path"
// argument. Extracting it lets a successful inspect call be matched against
// a later no-changes-needed evidence path. Malformed/missing "path" simply
// yields no trackable path - the call still counts toward
// attemptSuccessfulInspectionCalls, it just cannot back any evidence item.
std::optional<std::string> extractInspectedPathFromArguments(
    const std::string& argumentsJson)
{
    try {
        const auto arguments = nlohmann::json::parse(argumentsJson);
        if (!arguments.is_object() || !arguments.contains("path")
            || !arguments.at("path").is_string()) {
            return std::nullopt;
        }
        return normalizeComparablePath(arguments.at("path").get<std::string>());
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

// Local candidate produced by evaluating either terminal submission. Kept
// private to this translation unit: only the resolved disposition, manifest,
// or no-op payload are ever surfaced through ExecutionManifestPlannerOutcome.
struct PlanningCandidate {
    PlanningDisposition disposition {PlanningDisposition::ManifestReady};
    std::optional<ExecutionManifest> manifest;
    std::optional<PlannerNoChangesNeeded> noChanges;
    std::optional<PlannerVerificationActionRequired> verificationActionRequired;
};

bool equalPathList(
    const std::vector<std::filesystem::path>& left,
    const std::vector<std::filesystem::path>& right)
{
    if (left.size() != right.size()) return false;
    for (std::size_t index = 0; index < left.size(); ++index) {
        if (detail::genericUtf8(left[index]) != detail::genericUtf8(right[index])) return false;
    }
    return true;
}

bool equalAnnotatedPaths(
    const std::vector<ManifestExcludedOrDeferredPath>& left,
    const std::vector<ManifestExcludedOrDeferredPath>& right)
{
    if (left.size() != right.size()) return false;
    for (std::size_t index = 0; index < left.size(); ++index) {
        if (detail::genericUtf8(left[index].path) != detail::genericUtf8(right[index].path)
            || left[index].reason != right[index].reason) {
            return false;
        }
    }
    return true;
}

bool equalAcceptanceContract(
    const ExecutionAcceptanceContract& left,
    const ExecutionAcceptanceContract& right)
{
    if (left.schemaVersion != right.schemaVersion
        || left.requirements.size() != right.requirements.size()) {
        return false;
    }
    for (std::size_t index = 0; index < left.requirements.size(); ++index) {
        const auto& lhs = left.requirements[index];
        const auto& rhs = right.requirements[index];
        if (lhs.id != rhs.id || lhs.description != rhs.description
            || lhs.requiredOutputs != rhs.requiredOutputs
            || lhs.verificationKind != rhs.verificationKind
            || lhs.required != rhs.required
            || lhs.verificationProfileId != rhs.verificationProfileId
            || lhs.verificationBuildProfileId != rhs.verificationBuildProfileId
            || lhs.verificationTargetId != rhs.verificationTargetId
            || lhs.verificationBootstrapPending != rhs.verificationBootstrapPending
            || lhs.verificationArguments.size() != rhs.verificationArguments.size()) {
            return false;
        }
        for (std::size_t argument = 0;
             argument < lhs.verificationArguments.size(); ++argument) {
            if (lhs.verificationArguments[argument].name
                    != rhs.verificationArguments[argument].name
                || lhs.verificationArguments[argument].value
                    != rhs.verificationArguments[argument].value) {
                return false;
            }
        }
    }
    return true;
}

bool equalCoordinationTask(
    const ManifestDraftCoordinationTask& left,
    const ManifestDraftCoordinationTask& right)
{
    return left.id == right.id && left.objective == right.objective
        && left.role == right.role && left.dependencies == right.dependencies;
}

bool equalWorkTask(const ManifestDraftWorkTask& left, const ManifestDraftWorkTask& right)
{
    return left.id == right.id && left.objective == right.objective
        && left.role == right.role && left.dependencies == right.dependencies
        && equalPathList(left.writePaths, right.writePaths)
        && equalPathList(left.baselineRequiredReadPaths, right.baselineRequiredReadPaths)
        && equalPathList(left.proposedDependencyReadPaths, right.proposedDependencyReadPaths)
        && equalPathList(left.referencePaths, right.referencePaths)
        && equalAnnotatedPaths(left.excludedPaths, right.excludedPaths)
        && equalAnnotatedPaths(left.deferredPaths, right.deferredPaths)
        && left.acceptanceCriteria == right.acceptanceCriteria
        && left.contextReferences == right.contextReferences
        && left.requestedCapability == right.requestedCapability
        && left.assigneeMemberId == right.assigneeMemberId
        && equalAcceptanceContract(left.acceptanceContract, right.acceptanceContract);
}

bool equalBootstrapDeclaration(
    const std::optional<ManifestBootstrapDeclaration>& left,
    const std::optional<ManifestBootstrapDeclaration>& right)
{
    if (left.has_value() != right.has_value()) return false;
    if (!left.has_value()) return true;
    return left->bootstrapTaskId == right->bootstrapTaskId
        && equalPathList(left->setupPaths, right->setupPaths);
}

std::string describeOptionalNumber(const std::optional<std::uint64_t>& value)
{
    return value.has_value() ? std::to_string(*value) : std::string("null");
}

// Names the first top-level control a repair changed, for the audit trail of
// planner.repair_regressed (Run de6d81fb: the generic message could not say which of
// seven comparisons fired). Only Backend-parsed numbers and counts are quoted; the
// schemaVersion string and the bootstrap declaration are provider-authored, so they are
// reported by name or presence only, never by content.
std::optional<std::string> firstTopLevelControlChange(
    const ExecutionManifestDraft& primary, const ExecutionManifestDraft& repair)
{
    const auto changed = [](std::string_view field, const std::string& before,
                             const std::string& after) {
        return std::optional<std::string> {
            "Planner repair changed " + std::string(field) + ": " + before + " -> " + after};
    };
    if (primary.schemaVersion != repair.schemaVersion) {
        return "Planner repair changed schemaVersion";
    }
    if (primary.requestedMaxParallelism != repair.requestedMaxParallelism) {
        return changed("requestedMaxParallelism",
            std::to_string(primary.requestedMaxParallelism),
            std::to_string(repair.requestedMaxParallelism));
    }
    if (primary.requestedTokenBudget != repair.requestedTokenBudget) {
        return changed("requestedTokenBudget",
            describeOptionalNumber(primary.requestedTokenBudget),
            describeOptionalNumber(repair.requestedTokenBudget));
    }
    if (primary.requestedCostMicros != repair.requestedCostMicros) {
        return changed("requestedCostMicros",
            describeOptionalNumber(primary.requestedCostMicros),
            describeOptionalNumber(repair.requestedCostMicros));
    }
    if (!equalBootstrapDeclaration(
            primary.bootstrapDeclaration, repair.bootstrapDeclaration)) {
        const auto presence = [](const std::optional<ManifestBootstrapDeclaration>& value) {
            return std::string(value.has_value() ? "present" : "absent");
        };
        return changed("bootstrapDeclaration", presence(primary.bootstrapDeclaration),
            presence(repair.bootstrapDeclaration));
    }
    if (primary.coordinationTasks.size() != repair.coordinationTasks.size()) {
        return changed("coordinationTasks count",
            std::to_string(primary.coordinationTasks.size()),
            std::to_string(repair.coordinationTasks.size()));
    }
    if (primary.workTasks.size() != repair.workTasks.size()) {
        return changed("workTasks count", std::to_string(primary.workTasks.size()),
            std::to_string(repair.workTasks.size()));
    }
    return std::nullopt;
}

// MW-P3A package 3 (Monotonic Repair): when every primary finding names a
// concrete task, the repair may change those tasks but must preserve the
// complete allocation and contract of every other task, as well as the
// top-level draft controls. Findings without a task id are deliberately
// treated as graph/global: fixing empty_tasks, task-count/parallelism, or a
// graph-wide semantic predicate can legitimately reshape the whole draft.
// dependency_cycle is also graph-wide even though the validator attaches a
// task id for diagnostics; resolving it can require changing more than that
// one node. This check consumes parsed typed drafts only and never retains or
// compares raw provider JSON.
common::Result<void> validateMonotonicDraftChange(
    const ExecutionManifestDraft& primary,
    const ExecutionManifestDraft& repair,
    const std::vector<ExecutionManifestFinding>& primaryFindings)
{
    if (isGraphWideRepair(primaryFindings) || primaryFindings.empty()) {
        return common::Result<void>::ok();
    }

    std::unordered_set<std::string> affectedTaskIds;
    for (const auto& finding : primaryFindings)
        affectedTaskIds.insert(*finding.taskId);

    if (const auto change = firstTopLevelControlChange(primary, repair)) {
        return common::Result<void>::failure({"planner.repair_regressed", *change});
    }

    for (std::size_t index = 0; index < primary.coordinationTasks.size(); ++index) {
        const auto& before = primary.coordinationTasks[index];
        const auto& after = repair.coordinationTasks[index];
        if (before.id != after.id
            || (!affectedTaskIds.contains(before.id)
                && !equalCoordinationTask(before, after))) {
            return common::Result<void>::failure({
                "planner.repair_regressed",
                "Planner repair rewrote a coordination task unrelated to the primary findings"});
        }
    }
    for (std::size_t index = 0; index < primary.workTasks.size(); ++index) {
        const auto& before = primary.workTasks[index];
        const auto& after = repair.workTasks[index];
        if (before.id != after.id
            || (!affectedTaskIds.contains(before.id) && !equalWorkTask(before, after))) {
            return common::Result<void>::failure({
                "planner.repair_regressed",
                "Planner repair rewrote a work task unrelated to the primary findings"});
        }
    }
    return common::Result<void>::ok();
}

// Stable identity for a Backend-owned finding, used by the monotonic repair
// guard below to tell "the repair reproduced the same unresolved defect"
// apart from "the repair introduced a NEW, different defect". Most finding
// codes vary only by (code, taskId): a repair session resubmitting a
// manifest that still has the same defect reproduces the same (code,
// taskId) pair, and comparing on that pair alone is enough.
// manifest.conflicting_path_role is different: its (code, taskId) pair is
// shared by every conflicting path within one task, so a repair that leaves
// the originally-reported path conflicted AND creates a NEW conflicting
// path/role pair in the same task would otherwise look identical to "no
// progress" instead of the regression it actually is (real Run
// ed9b84fc-f0fd-4796-b7d2-743715247be1, 2026-09-27: 1 conflicting path grew
// to 6 on the one bounded repair, but the outcome was still reported as
// planner.repair_exhausted rather than planner.repair_regressed). This
// finding's message is fully deterministic - built only from the normalized
// path and the two WorkspaceScopeRole labels, never from provider text (see
// ExecutionManifestValidator.cpp's diagnoseAndTrack) - so folding it into
// the identity for this code is safe and introduces no provider-text
// parsing. manifest.missing_field is also safe: ManifestJsonParsingSupport
// builds its message solely from the Backend-owned structural field
// location. Do not extend this to provider-authored or incidental wording:
// most other finding messages can vary without representing a new defect.
struct FindingIdentity {
    std::string code;
    std::optional<std::string> taskId;
    std::optional<std::string> deterministicDetail;

    bool operator==(const FindingIdentity&) const = default;
};

FindingIdentity findingIdentity(const ExecutionManifestFinding& finding)
{
    const bool hasDeterministicDetail =
        finding.code == "manifest.conflicting_path_role"
        || finding.code == "manifest.missing_field";
    return {
        finding.code,
        finding.taskId,
        hasDeterministicDetail
            ? std::optional<std::string> {finding.message}
            : std::nullopt};
}

bool introducesUnrelatedFinding(
    const std::vector<ExecutionManifestFinding>& primaryFindings,
    std::string_view primaryErrorCode,
    const std::vector<ExecutionManifestFinding>& repairFindings,
    std::string_view repairErrorCode)
{
    if (primaryFindings.empty()) return primaryErrorCode != repairErrorCode;
    if (repairFindings.empty()) {
        return std::none_of(primaryFindings.begin(), primaryFindings.end(),
            [&](const auto& primary) { return primary.code == repairErrorCode; });
    }
    std::vector<FindingIdentity> primaryIdentities;
    primaryIdentities.reserve(primaryFindings.size());
    for (const auto& primary : primaryFindings) {
        primaryIdentities.push_back(findingIdentity(primary));
    }
    return std::any_of(repairFindings.begin(), repairFindings.end(),
        [&](const auto& repaired) {
            const auto repairedIdentity = findingIdentity(repaired);
            return std::find(primaryIdentities.begin(), primaryIdentities.end(),
                       repairedIdentity)
                == primaryIdentities.end();
        });
}

constexpr std::size_t kMaximumNoChangesSubmissionBytes = 2ULL * 1024 * 1024;
constexpr std::size_t kMaximumNoChangesEvidenceItems = 32;
constexpr std::size_t kMaximumNoChangesStringBytes = 64ULL * 1024;

template <typename T>
common::Result<T> failNoChanges(std::string code, std::string message)
{
    return common::Result<T>::failure({std::move(code), std::move(message)});
}

// Parses and validates a submit_no_changes_needed payload. The provider is
// never trusted: every field required by the contract (Package/requirement 3
// in the fix plan this implements) is re-checked here regardless of what the
// advertised JSON schema already constrains. Path safety is checked against
// the same trusted inspectPolicy the session itself was bounded by.
common::Result<PlannerNoChangesNeeded> parseNoChangesSubmission(
    const std::string& rawSubmission,
    const WorkspaceReadPolicy& inspectPolicy)
{
    if (rawSubmission.size() > kMaximumNoChangesSubmissionBytes) {
        return failNoChanges<PlannerNoChangesNeeded>(
            "planner.no_changes_output_too_large",
            "No-changes-needed submission exceeds the parser byte limit");
    }

    nlohmann::json document;
    try {
        document = nlohmann::json::parse(rawSubmission);
    } catch (const std::exception&) {
        return failNoChanges<PlannerNoChangesNeeded>(
            "planner.no_changes_parse_error",
            "No-changes-needed submission is not valid JSON");
    }
    if (!document.is_object()) {
        return failNoChanges<PlannerNoChangesNeeded>(
            "planner.no_changes_parse_error",
            "No-changes-needed submission must be a JSON object");
    }

    static const std::unordered_set<std::string> allowedFields {
        "schemaVersion", "summary", "evidence", "recommendedNextStep"};
    for (const auto& [key, unused] : document.items()) {
        static_cast<void>(unused);
        if (!allowedFields.contains(key)) {
            return failNoChanges<PlannerNoChangesNeeded>(
                "planner.no_changes_unexpected_field",
                "No-changes-needed submission contains an unexpected field");
        }
    }

    if (!document.contains("schemaVersion") || !document.at("schemaVersion").is_string()
        || document.at("schemaVersion").get<std::string>()
               != std::string(kPlannerNoChangesNeededSchemaVersion)) {
        return failNoChanges<PlannerNoChangesNeeded>(
            "planner.no_changes_unsupported_schema",
            "No-changes-needed submission schema version is not supported");
    }

    if (!document.contains("summary") || !document.at("summary").is_string()) {
        return failNoChanges<PlannerNoChangesNeeded>(
            "planner.no_changes_invalid_field", "summary must be a string");
    }
    auto summary = document.at("summary").get<std::string>();
    if (isBlank(summary) || summary.size() > kMaximumNoChangesStringBytes) {
        return failNoChanges<PlannerNoChangesNeeded>(
            "planner.no_changes_empty_summary",
            "No-changes-needed submission summary cannot be blank");
    }

    if (!document.contains("evidence") || !document.at("evidence").is_array()
        || document.at("evidence").empty()) {
        return failNoChanges<PlannerNoChangesNeeded>(
            "planner.no_changes_empty_evidence",
            "No-changes-needed submission requires at least one bounded evidence item");
    }
    const auto& evidenceArray = document.at("evidence");
    if (evidenceArray.size() > kMaximumNoChangesEvidenceItems) {
        return failNoChanges<PlannerNoChangesNeeded>(
            "planner.no_changes_evidence_limit_exceeded",
            "No-changes-needed submission exceeds the evidence item limit");
    }

    PlannerNoChangesNeeded result;
    result.schemaVersion = std::string(kPlannerNoChangesNeededSchemaVersion);
    result.summary = std::move(summary);
    result.evidence.reserve(evidenceArray.size());

    for (const auto& item : evidenceArray) {
        if (!item.is_object()
            || !item.contains("criterion") || !item.at("criterion").is_string()
            || !item.contains("path") || !item.at("path").is_string()
            || !item.contains("observation") || !item.at("observation").is_string()) {
            return failNoChanges<PlannerNoChangesNeeded>(
                "planner.no_changes_invalid_evidence",
                "No-changes-needed evidence item has an invalid shape");
        }
        for (const auto& [key, unused] : item.items()) {
            static_cast<void>(unused);
            if (key != "criterion" && key != "path" && key != "observation") {
                return failNoChanges<PlannerNoChangesNeeded>(
                    "planner.no_changes_invalid_evidence",
                    "No-changes-needed evidence item contains an unexpected field");
            }
        }

        auto criterion = item.at("criterion").get<std::string>();
        auto pathText = item.at("path").get<std::string>();
        auto observation = item.at("observation").get<std::string>();
        if (isBlank(criterion) || isBlank(pathText) || isBlank(observation)
            || criterion.size() > kMaximumNoChangesStringBytes
            || pathText.size() > kMaximumNoChangesStringBytes
            || observation.size() > kMaximumNoChangesStringBytes) {
            return failNoChanges<PlannerNoChangesNeeded>(
                "planner.no_changes_invalid_evidence",
                "No-changes-needed evidence item fields cannot be blank");
        }
        const auto safePath = WorkspaceReadPolicyValidator::validateRelativePath(
            std::filesystem::path(pathText), inspectPolicy);
        if (!safePath.success()) {
            return failNoChanges<PlannerNoChangesNeeded>(
                "planner.no_changes_invalid_evidence_path",
                "No-changes-needed evidence path is not a safe workspace-relative path");
        }

        result.evidence.push_back(
            {std::move(criterion), std::move(pathText), std::move(observation)});
    }

    if (document.contains("recommendedNextStep") && !document.at("recommendedNextStep").is_null()) {
        if (!document.at("recommendedNextStep").is_string()) {
            return failNoChanges<PlannerNoChangesNeeded>(
                "planner.no_changes_invalid_field",
                "recommendedNextStep must be a string or null when present");
        }
        auto recommendation = document.at("recommendedNextStep").get<std::string>();
        if (recommendation.size() > kMaximumNoChangesStringBytes) {
            return failNoChanges<PlannerNoChangesNeeded>(
                "planner.no_changes_field_too_large",
                "recommendedNextStep exceeds the parser byte limit");
        }
        if (!isBlank(recommendation)) {
            result.recommendedNextStep = std::move(recommendation);
        }
    }

    return common::Result<PlannerNoChangesNeeded>::ok(std::move(result));
}

template <typename T>
common::Result<T> failActionRequired(std::string code, std::string message)
{
    return common::Result<T>::failure({std::move(code), std::move(message)});
}

// AG-18.3 hardening (blocker 2): parses/validates a
// submit_verification_action_required payload. No path/command/preset-JSON/
// secret can appear here structurally - the schema only ever allows bounded
// prose strings, never a nested object or array.
common::Result<PlannerVerificationActionRequired> parseVerificationActionRequiredSubmission(
    const std::string& rawSubmission)
{
    if (rawSubmission.size() > kMaximumNoChangesSubmissionBytes) {
        return failActionRequired<PlannerVerificationActionRequired>(
            "planner.verification_action_required_output_too_large",
            "Verification-action-required submission exceeds the parser byte limit");
    }
    nlohmann::json document;
    try {
        document = nlohmann::json::parse(rawSubmission);
    } catch (const std::exception&) {
        return failActionRequired<PlannerVerificationActionRequired>(
            "planner.verification_action_required_parse_error",
            "Verification-action-required submission is not valid JSON");
    }
    if (!document.is_object()) {
        return failActionRequired<PlannerVerificationActionRequired>(
            "planner.verification_action_required_parse_error",
            "Verification-action-required submission must be a JSON object");
    }
    static const std::unordered_set<std::string> allowedFields {
        "schemaVersion", "summary", "reason", "recommendedNextStep"};
    for (const auto& [key, unused] : document.items()) {
        static_cast<void>(unused);
        if (!allowedFields.contains(key)) {
            return failActionRequired<PlannerVerificationActionRequired>(
                "planner.verification_action_required_unexpected_field",
                "Verification-action-required submission contains an unexpected field");
        }
    }
    if (!document.contains("schemaVersion") || !document.at("schemaVersion").is_string()
        || document.at("schemaVersion").get<std::string>()
               != std::string(kPlannerVerificationActionRequiredSchemaVersion)) {
        return failActionRequired<PlannerVerificationActionRequired>(
            "planner.verification_action_required_unsupported_schema",
            "Verification-action-required submission schema version is not supported");
    }
    if (!document.contains("summary") || !document.at("summary").is_string()) {
        return failActionRequired<PlannerVerificationActionRequired>(
            "planner.verification_action_required_invalid_field", "summary must be a string");
    }
    auto summary = document.at("summary").get<std::string>();
    if (isBlank(summary) || summary.size() > kMaximumNoChangesStringBytes) {
        return failActionRequired<PlannerVerificationActionRequired>(
            "planner.verification_action_required_empty_summary",
            "Verification-action-required submission summary cannot be blank");
    }
    if (!document.contains("reason") || !document.at("reason").is_string()) {
        return failActionRequired<PlannerVerificationActionRequired>(
            "planner.verification_action_required_invalid_field", "reason must be a string");
    }
    auto reason = document.at("reason").get<std::string>();
    if (isBlank(reason) || reason.size() > kMaximumNoChangesStringBytes) {
        return failActionRequired<PlannerVerificationActionRequired>(
            "planner.verification_action_required_empty_reason",
            "Verification-action-required submission reason cannot be blank");
    }

    PlannerVerificationActionRequired result;
    result.schemaVersion = std::string(kPlannerVerificationActionRequiredSchemaVersion);
    result.summary = std::move(summary);
    result.reason = std::move(reason);
    if (document.contains("recommendedNextStep") && !document.at("recommendedNextStep").is_null()) {
        if (!document.at("recommendedNextStep").is_string()) {
            return failActionRequired<PlannerVerificationActionRequired>(
                "planner.verification_action_required_invalid_field",
                "recommendedNextStep must be a string or null when present");
        }
        auto recommendation = document.at("recommendedNextStep").get<std::string>();
        if (recommendation.size() > kMaximumNoChangesStringBytes) {
            return failActionRequired<PlannerVerificationActionRequired>(
                "planner.verification_action_required_field_too_large",
                "recommendedNextStep exceeds the parser byte limit");
        }
        if (!isBlank(recommendation)) result.recommendedNextStep = std::move(recommendation);
    }
    return common::Result<PlannerVerificationActionRequired>::ok(std::move(result));
}

} // namespace

bool isNonRepairableSubmissionFailure(std::string_view errorCode) noexcept
{
    return isMandatoryVerificationPolicyFailureCode(errorCode);
}

AgentToolDefinition submitExecutionManifestToolDefinition()
{
    AgentToolDefinition definition;
    definition.name = kSubmitExecutionManifestToolName;
    definition.version = "1";
    definition.description =
        "Submit the final execution-manifest.v1 task graph; this ends planning";
    // Strict-compatible (OpenAI Responses / Anthropic Messages strict
    // subset): acceptanceContract is unconditionally required as a key on
    // every task, typed object|null - the conditional "inspect/propose
    // needs a real contract" business rule is enforced by the Backend after
    // parse (see the requestedCapability/acceptanceContract check below),
    // never expressed via allOf/if/then, which neither provider's strict
    // mode supports.
    definition.inputSchemaJson = R"JSON({
"type":"object",
"properties":{
  "schemaVersion":{"type":"string","const":"execution-manifest.v1"},
  "tasks":{"type":"array","minItems":1,"maxItems":64,"description":"Only concrete remaining execution work; never repository-audit or report-only filler tasks","items":{"type":"object","properties":{
    "id":{"type":"string","minLength":1},"objective":{"type":"string","minLength":1},"role":{"type":"string","minLength":1},
    "dependencies":{"type":"array","items":{"type":"string"}},
    "ownedPaths":{"type":"array","maxItems":256,"description":"Clean disjoint workspace-relative paths with forward slashes; no root, dot segments, absolute paths, backslashes, or globs","items":{"type":"string","minLength":1}},
    "plannedCreatePaths":{"type":"array","maxItems":256,"description":"New workspace-relative files this task may create; use an empty array when none","items":{"type":"string","minLength":1}},
    "requiredReadPaths":{"type":"array","maxItems":256,"description":"Existing workspace-relative paths this task must inspect in its own attempt; use an empty array when none","items":{"type":"string","minLength":1}},
    "referencePaths":{"type":"array","maxItems":256,"description":"Read-only workspace-relative reference paths; use an empty array when none","items":{"type":"string","minLength":1}},
    "excludedPaths":{"type":"array","maxItems":256,"description":"Deliberately excluded workspace-relative paths with a nonblank audit reason; use an empty array when none","items":{"type":"object","properties":{"path":{"type":"string","minLength":1},"reason":{"type":"string","minLength":1,"maxLength":2000}},"required":["path","reason"],"additionalProperties":false}},
    "deferredPaths":{"type":"array","maxItems":256,"description":"Deliberately deferred workspace-relative paths with a nonblank audit reason; use an empty array when none","items":{"type":"object","properties":{"path":{"type":"string","minLength":1},"reason":{"type":"string","minLength":1,"maxLength":2000}},"required":["path","reason"],"additionalProperties":false}},
    "acceptanceCriteria":{"type":"array","maxItems":256,"items":{"type":"string","minLength":1}},
    "acceptanceContract":{"type":["object","null"],"description":"A real execution-acceptance.v1 object for requestedCapability inspect/propose; the JSON literal null only for none.","properties":{
      "schemaVersion":{"type":"string","const":"execution-acceptance.v1"},
      "requirements":{"type":"array","minItems":1,"maxItems":64,"items":{"type":"object","properties":{
        "id":{"type":"string","minLength":1,"maxLength":4096},
        "description":{"type":"string","minLength":1,"maxLength":4096},
        "requiredOutputs":{"type":"array","description":"Exact files from this same task's ownedPaths, plannedCreatePaths, or requiredReadPaths; never report labels or descriptive phrases.","maxItems":32,"items":{"type":"string","minLength":1,"maxLength":4096}},
        "verificationKind":{"type":"string","description":"Backend profile. inspect_paths needs no profile/build-profile selection; cmake_build/qml_lint select a catalog profile+build-profile; qml_lint also selects a catalog target.","enum":["inspect_paths","cmake_build","qml_lint"]},
        "verificationProfileId":{"type":["string","null"],"description":"A profileId taken verbatim from the trusted verification environment catalog. Required for cmake_build/qml_lint; must be null for inspect_paths. Never invent an id not advertised in the catalog.","maxLength":128},
        "verificationBuildProfileId":{"type":["string","null"],"description":"A buildProfileId taken verbatim from the selected profile's own catalog entry. Required for cmake_build/qml_lint; must be null for inspect_paths. Never invent an id not advertised under the selected profile.","maxLength":128},
        "verificationTargetId":{"type":["string","null"],"description":"A target id taken verbatim from a confirmed catalog allow-list. Required for qml_lint (from the selected build profile); must be null for inspect_paths/cmake_build. Never invent a target not advertised in the catalog.","maxLength":128},
        "verificationArguments":{"type":"array","description":"Task-owned arguments only. Always empty [] - the Backend resolves inspect_paths/cmake_build/qml_lint environment arguments exclusively from your profile/build-profile/target selection and a non-empty array here is rejected as a forbidden raw override.","maxItems":16,"items":{"type":"object","properties":{"name":{"type":"string","minLength":1,"maxLength":4096},"value":{"type":"string","maxLength":4096}},"required":["name","value"],"additionalProperties":false}},
        "required":{"type":"boolean"},
        "verificationBootstrapPending":{"type":"boolean","description":"true ONLY when no existing verification profile is available and this exact requirement's environment must be resolved later from the manifest-level bootstrapDeclaration's candidate proposal. When true, verificationProfileId/verificationBuildProfileId/verificationTargetId must all be null and verificationArguments must be empty. Never true unless bootstrapDeclaration is also set; never true for inspect_paths."}
      },"required":["id","description","requiredOutputs","verificationKind","verificationProfileId","verificationBuildProfileId","verificationTargetId","verificationArguments","required","verificationBootstrapPending"],"additionalProperties":false}}
    },"required":["schemaVersion","requirements"],"additionalProperties":false},
    "contextReferences":{"type":"array","maxItems":256,"items":{"type":"string","minLength":1}},
    "requestedCapability":{"type":"string","enum":["none","inspect","propose"]}
  },"required":["id","objective","role","dependencies","ownedPaths","plannedCreatePaths","requiredReadPaths","referencePaths","excludedPaths","deferredPaths","acceptanceCriteria","acceptanceContract","contextReferences","requestedCapability"],"additionalProperties":false}},
  "requestedMaxParallelism":{"type":"integer","minimum":1,"maximum":32},
  "requestedTokenBudget":{"type":["integer","null"],"minimum":0},
  "requestedCostMicros":{"type":["integer","null"],"minimum":0},
  "bootstrapDeclaration":{"type":["object","null"],"description":"Set ONLY when no existing verification profile is available and this Manifest creates or modifies the exact CMake preset file(s) needed to establish one, in this same Run, as ordinary reviewable owned/planned-create work. JSON null when no bootstrap is declared - never inferred from any other field.","properties":{
    "bootstrapTaskId":{"type":"string","minLength":1,"description":"The single task that owns every declared setup file, via its own ownedPaths (file already exists) or plannedCreatePaths (new file)."},
    "setupPaths":{"type":"array","minItems":1,"maxItems":4,"description":"Exact repository-root paths, only CMakePresets.json and/or CMakeUserPresets.json - never a subdirectory, never any other filename.","items":{"type":"string","minLength":1}}
  },"required":["bootstrapTaskId","setupPaths"],"additionalProperties":false}
},
"required":["schemaVersion","tasks","requestedMaxParallelism","requestedTokenBudget","requestedCostMicros","bootstrapDeclaration"],
"additionalProperties":false
})JSON";
    definition.readOnly = true;
    definition.maxResultBytes = 4096;
    definition.strictInputSchema = true;
    return definition;
}

AgentToolDefinition submitNoChangesNeededToolDefinition()
{
    AgentToolDefinition definition;
    definition.name = kSubmitNoChangesNeededToolName;
    definition.version = "1";
    definition.description =
        "Declare that the current Run task's acceptance criteria are already "
        "satisfied and no Execution Manifest is needed; ends planning without "
        "freezing a manifest";
    definition.inputSchemaJson = R"JSON({
"type":"object",
"properties":{
  "schemaVersion":{"type":"string","const":"execution-manifest-no-changes.v1"},
  "summary":{"type":"string","minLength":1},
  "evidence":{"type":"array","minItems":1,"maxItems":32,"items":{"type":"object","properties":{
    "criterion":{"type":"string","minLength":1},
    "path":{"type":"string","minLength":1},
    "observation":{"type":"string","minLength":1}},
    "required":["criterion","path","observation"],
    "additionalProperties":false}},
  "recommendedNextStep":{"type":["string","null"]}
},
"required":["schemaVersion","summary","evidence","recommendedNextStep"],
"additionalProperties":false
})JSON";
    definition.readOnly = true;
    definition.maxResultBytes = 4096;
    definition.strictInputSchema = true;
    return definition;
}

AgentToolDefinition submitVerificationActionRequiredToolDefinition()
{
    AgentToolDefinition definition;
    definition.name = kSubmitVerificationActionRequiredToolName;
    definition.version = "1";
    definition.description =
        "Declare that the immutable verification environment snapshot has no usable "
        "profile and this Run cannot safely form a same-Run bootstrap either; ends "
        "planning with a durable, non-green disposition and dispatches no worker. "
        "The Backend accepts this only when the snapshot genuinely has no usable "
        "profile - never call it when an existing profile is available, and never as "
        "a substitute for a valid bootstrapDeclaration you could otherwise form.";
    definition.inputSchemaJson = R"JSON({
"type":"object",
"properties":{
  "schemaVersion":{"type":"string","const":"verification-action-required.v1"},
  "summary":{"type":"string","minLength":1},
  "reason":{"type":"string","minLength":1},
  "recommendedNextStep":{"type":["string","null"]}
},
"required":["schemaVersion","summary","reason","recommendedNextStep"],
"additionalProperties":false
})JSON";
    definition.readOnly = true;
    definition.maxResultBytes = 4096;
    definition.strictInputSchema = true;
    return definition;
}

ExecutionManifestPlanner::ExecutionManifestPlanner(
    providers::IProviderAgentSessionFactory& sessionFactory,
    AgentToolExecutionService& toolExecution,
    ExecutionManifestService& manifests)
    : sessionFactory_(sessionFactory)
    , toolExecution_(toolExecution)
    , manifests_(manifests)
{
}

ExecutionManifestPlannerOutcome ExecutionManifestPlanner::plan(
    ExecutionManifestPlannerRequest request,
    std::stop_token stopToken) const
{
    ExecutionManifestPlannerOutcome outcome;
    if (request.runId.empty()) {
        outcome.errorCode = "planner.empty_run_id";
        outcome.errorMessage = "Planner requires a trusted Run identifier";
        return outcome;
    }
    if (request.providerKind.empty()) {
        outcome.errorCode = "planner.empty_provider_kind";
        outcome.errorMessage = "Planner requires a trusted provider kind";
        return outcome;
    }
    // AG-17.1: the routing record is Backend-owned; a defective one is a Backend
    // bug and must fail before any provider session, never reach the repair.
    if (request.trustedRouting.has_value()) {
        if (const auto problem = ManifestRouting::recordProblem(*request.trustedRouting)) {
            outcome.errorCode = "planner.routing_invalid";
            outcome.errorMessage = *problem;
            return outcome;
        }
    }
    // AG-17.4: the roster the Planner is shown must be exactly the roster the
    // record froze. Both are Backend-built, so any disagreement is a Backend
    // defect and ends planning before a provider session exists.
    std::vector<std::string> assignableMemberIds;
    std::string rosterSection;
    if (request.trustedRouting.has_value()) {
        if (const auto problem = ManifestRouting::rosterCatalogProblem(
                *request.trustedRouting, request.rosterCatalog)) {
            outcome.errorCode = "planner.roster_catalog_invalid";
            outcome.errorMessage = *problem;
            return outcome;
        }
        assignableMemberIds = ManifestRouting::assignableMemberIds(*request.trustedRouting);
        if (request.trustedRouting->mode == ManifestRoutingMode::Roster) {
            if (assignableMemberIds.empty()) {
                outcome.errorCode = "planner.roster_no_assignable_member";
                outcome.errorMessage =
                    "The roster has no worker with workspace access, so no task could be assigned";
                return outcome;
            }
            rosterSection = ManifestRouting::renderRosterCatalog(
                *request.trustedRouting, request.rosterCatalog);
        }
    } else if (!request.rosterCatalog.empty()) {
        outcome.errorCode = "planner.roster_catalog_invalid";
        outcome.errorMessage = "A roster catalog needs the Backend routing record it describes";
        return outcome;
    }
    if (const auto policy = WorkspaceReadPolicyValidator::validateTrustedPolicy(
            request.inspectPolicy);
        !policy.success()) {
        outcome.errorCode = policy.error()->code;
        outcome.errorMessage = policy.error()->message;
        return outcome;
    }
    if (const auto projection = validateProjection(request.repositoryProjection);
        !projection.success()) {
        outcome.errorCode = projection.error()->code;
        outcome.errorMessage = projection.error()->message;
        return outcome;
    }
    if (request.repositoryProjection.has_value()) {
        for (const auto& path : request.repositoryProjection->changedPaths) {
            const auto safePath = WorkspaceReadPolicyValidator::validateRelativePath(
                std::filesystem::path(path), request.inspectPolicy);
            if (!safePath.success()) {
                outcome.errorCode = "planner.invalid_repository_projection";
                outcome.errorMessage =
                    "Repository inspection projection contains an unsafe path";
                return outcome;
            }
        }
    }

    auto& sessionRequest = request.sessionRequest;
    if (request.providerBudgetGuard) {
        sessionRequest.turnTimeout = request.providerBudgetGuard->boundedRequestTimeout(
            sessionRequest.turnTimeout);
    }
    sessionRequest.systemPrompt = std::string(kPlannerSystemPrompt)
        + (sessionRequest.systemPrompt.empty()
            ? std::string {}
            : "\n\n# Additional trusted planning context\n" + sessionRequest.systemPrompt)
        + (rosterSection.empty() ? std::string {} : "\n\n" + rosterSection)
        + (request.allowEvidenceBackedNoChanges
            ? "\n\n# Trusted completion policy\n"
              "This Run explicitly allows submit_no_changes_needed when all of its "
              "current acceptance criteria are already satisfied and the submission "
              "passes every same-session inspection/evidence check above."
            : "\n\n# Trusted completion policy\n"
              "This implementation Run requires submit_execution_manifest_draft. "
              "submit_no_changes_needed is not advertised and is not an accepted "
              "terminal outcome. Do not claim successful completion without a "
              "non-empty, valid execution Manifest. Do not fabricate audit-only or "
              "report-only tasks; if no concrete work truly remains, the Run must "
              "fail closed rather than complete as no changes needed.");
    sessionRequest.userContent = appendProjection(
        std::move(sessionRequest.userContent), request.repositoryProjection);
    sessionRequest.tools = toolExecution_.toolDefinitions();
    sessionRequest.tools.push_back(
        submitExecutionManifestDraftToolDefinition(assignableMemberIds));
    if (request.allowEvidenceBackedNoChanges)
        sessionRequest.tools.push_back(submitNoChangesNeededToolDefinition());
    // AG-18.3 hardening: always advertised (unlike submit_no_changes_needed,
    // this is never gated by planningCompletionPolicy) - the Backend itself
    // enforces applicability at submission time (verificationEnvironment
    // genuinely SetupRequired), so a Run that never reaches that state
    // simply never has a legitimate use for it.
    sessionRequest.tools.push_back(submitVerificationActionRequiredToolDefinition());

    if (const auto validRequest = providers::validateSessionRequest(sessionRequest);
        !validRequest.success()) {
        outcome.errorCode = validRequest.error()->code;
        outcome.errorMessage = validRequest.error()->message;
        return outcome;
    }

    // Package 1 (2026-09-28 planner-terminal-budget-hardening): partition the
    // frozen Run-wide provider turn ceiling BEFORE any provider dispatch, so
    // an insufficient budget fails closed with zero paid provider work
    // rather than repeating the ade2ca50.../7385ad56... incident pattern
    // (see ExecutionManifestPlannerTurnBudget.hpp). No guard means no known
    // Run-wide ceiling to partition - legacy direct unit callers that omit
    // providerBudgetGuard remain source-compatible and unrestricted, exactly
    // as before this hardening.
    std::optional<PlannerTurnBudgetPartition> turnBudgetPartition;
    if (request.providerBudgetGuard) {
        auto partitioned = partitionPlannerTurnBudget(
            request.providerBudgetGuard->policy().maxTurns);
        if (!partitioned.success()) {
            outcome.errorCode = partitioned.error()->code;
            outcome.errorMessage = partitioned.error()->message;
            return outcome;
        }
        turnBudgetPartition = partitioned.value();
    }

    auto firstSessionResult = sessionFactory_.createSession(
        sessionRequest.providerId, request.providerKind);
    if (!firstSessionResult.success()) {
        outcome.errorCode = firstSessionResult.error()->code;
        outcome.errorMessage = firstSessionResult.error()->message;
        return outcome;
    }

    AgentToolSession auditSession = AgentToolExecutionService::makeSession(
        request.runId, sessionRequest.providerId, sessionRequest.modelId);
    const auto beganAudit = toolExecution_.beginSession(auditSession);
    if (!beganAudit.success()) {
        outcome.errorCode = beganAudit.error()->code;
        outcome.errorMessage = beganAudit.error()->message;
        return outcome;
    }

    std::size_t executedCalls = 0;
    std::size_t totalCalls = 0;
    std::size_t successfulCalls = 0;
    std::size_t totalBytes = 0;
    std::size_t persistedTurns = 0;
    models::TokenUsage totalUsage;

    const auto finishAudit = [&](AgentToolSessionState state) {
        auditSession.state = state;
        auditSession.totalTurns = persistedTurns;
        auditSession.totalCalls = executedCalls;
        auditSession.totalReturnedBytes = totalBytes;
        auditSession.finishedAt = std::chrono::system_clock::now();
        static_cast<void>(toolExecution_.updateSession(auditSession));
    };
    const auto fillEvidence = [&] {
        outcome.totalTurns = persistedTurns;
        outcome.totalToolCalls = totalCalls;
        outcome.successfulInspectionCalls = successfulCalls;
        outcome.totalReturnedBytes = totalBytes;
        outcome.usage = totalUsage;
    };

    std::unordered_set<std::string> inspectTools;
    for (const auto& definition : toolExecution_.toolDefinitions())
        inspectTools.insert(definition.name);

    // Package 2 (2026-09-28 planner-terminal-budget-hardening): the exact
    // terminal tool set a Backend-forced turn may advertise - always
    // submit_execution_manifest_draft, plus the other two terminal tools
    // only when this Run's own contract actually makes them applicable.
    // Depends only on `request`, never on provider/model output, so it is
    // computed once and reused for every forced turn across the primary
    // attempt and any recovery/repair attempt.
    const auto forcedTerminalToolNames = [&]() -> std::vector<std::string> {
        std::vector<std::string> names {kSubmitExecutionManifestDraftToolName};
        if (request.allowEvidenceBackedNoChanges)
            names.push_back(kSubmitNoChangesNeededToolName);
        if (request.verificationEnvironment.has_value()
                && request.verificationEnvironment->state
                    == VerificationEnvironmentState::SetupRequired) {
            names.push_back(kSubmitVerificationActionRequiredToolName);
        }
        return names;
    };

    const auto runAttempt = [&](providers::IProviderAgentSession& providerSession,
                                providers::ProviderAgentSessionRequest attemptRequest)
        -> AttemptResult {
        // Local to this invocation: reset for every call to runAttempt, so a
        // repair session starts with zero of its own inspected evidence
        // regardless of what the primary attempt already inspected.
        std::size_t attemptSuccessfulCalls = 0;
        std::unordered_set<std::string> attemptInspectedPaths;
        // Mirrors the NextTurnToolPolicy (if any) that was sent for the turn
        // response currently being processed - std::nullopt for this
        // attempt's own first turn (beginSession() cannot carry a policy).
        // Package 2: once set, an incoming ToolUse response naming a tool
        // outside this policy's allowedToolNames is a fail-closed protocol
        // violation, never silently executed as further inspection.
        std::optional<providers::NextTurnToolPolicy> pendingTerminalPolicy;

        if (request.providerBudgetGuard) {
            const auto allowed = request.providerBudgetGuard->beginProviderTurn(stopToken);
            if (!allowed.success()) {
                return {false, false, false, {},
                        allowed.error()->code, allowed.error()->message};
            }
        }
        auto turnResult = providerSession.beginSession(attemptRequest, stopToken);
        if (!turnResult.success() && request.providerBudgetGuard) {
            request.providerBudgetGuard->abandonProviderTurn();
        }
        while (true) {
            if (!turnResult.success()) {
                return {false, false, false, {},
                        turnResult.error()->code, turnResult.error()->message};
            }
            const auto& turn = turnResult.value();
            const bool terminalWithoutUsage =
                (turn.finishReason == providers::ProviderAgentFinishReason::StopRequested
                    || turn.finishReason == providers::ProviderAgentFinishReason::Error)
                && turn.usage.inputTokens == 0 && turn.usage.outputTokens == 0;
            // Count the provider response locally even when it is the turn
            // that exhausts the Run-wide budget. The guard persists the same
            // usage before returning its typed stop.
            //
            // MW-P3/AG-18.5: inputTokens is the conservative total, but the
            // cache subsets must survive the Planner aggregation as well so
            // the caller can calculate an honest cache-aware cost for this
            // entire bounded planning session (omitting them once made
            // Anthropic Planner spend impossible to price even though Run-level
            // budget accounting had received the original per-turn usage).
            // AG-17 M2b: the ONE accumulator - shared with the worker turn loop -
            // also classifies each response against the pricing model's
            // long-context threshold, so the primary attempt and the bounded
            // repair are priced exactly per response. The guard below still
            // receives the plain turn.usage and never reads the threshold.
            accumulateResponseUsage(
                totalUsage, turn.usage, request.longContextThresholdInputTokens);
            if (request.providerBudgetGuard) {
                if (terminalWithoutUsage) {
                    request.providerBudgetGuard->abandonProviderTurn();
                } else {
                    const auto recorded = request.providerBudgetGuard->completeProviderTurn(turn.usage);
                    if (!recorded.success()) {
                        return {false, false, false, {},
                                recorded.error()->code, recorded.error()->message};
                    }
                }
            }
            AgentProviderTurn turnRecord;
            turnRecord.turnId = generateUuid();
            turnRecord.sessionId = auditSession.sessionId;
            turnRecord.turnIndex = persistedTurns++;
            turnRecord.finishReason = finishReasonText(turn.finishReason);
            turnRecord.inputTokens = static_cast<int>(turn.usage.inputTokens);
            turnRecord.outputTokens = static_cast<int>(turn.usage.outputTokens);
            turnRecord.callCount = turn.toolCalls.size();
            turnRecord.createdAt = std::chrono::system_clock::now();
            static_cast<void>(toolExecution_.recordTurn(std::move(turnRecord)));

            if (turn.finishReason == providers::ProviderAgentFinishReason::ToolUse) {
                // Package 2: defense-in-depth against a misbehaving/mocked
                // session that ignores the forced-terminal NextTurnToolPolicy
                // just sent. Real OpenAI/Anthropic turns cannot reach this -
                // both adapters filter the advertised tools to
                // allowedToolNames and require a tool call - but the Planner
                // itself must never execute an inspect tool call that
                // arrives on a turn it forced to terminal-only; that would
                // silently let inspection continue past the Backend-owned
                // boundary.
                if (pendingTerminalPolicy.has_value()) {
                    const bool everyCallAllowed = std::all_of(
                        turn.toolCalls.begin(), turn.toolCalls.end(),
                        [&](const AgentToolCall& call) {
                            const auto& allowed = pendingTerminalPolicy->allowedToolNames;
                            return std::find(allowed.begin(), allowed.end(), call.toolName)
                                != allowed.end();
                        });
                    if (!everyCallAllowed) {
                        return {false, false, false, {},
                                "planner.forced_terminal_turn_violated",
                                "Planner requested a tool outside the Backend-forced "
                                "terminal-only policy for this turn"};
                    }
                }
                totalCalls += turn.toolCalls.size();
                if (request.providerBudgetGuard) {
                    const auto recorded = request.providerBudgetGuard->recordToolActivity(
                        turn.toolCalls.size(), 0);
                    if (!recorded.success()) {
                        return {false, false, false, {},
                                recorded.error()->code, recorded.error()->message};
                    }
                }
                if (totalCalls > sessionRequest.maxTotalToolCalls) {
                    return {false, false, false, {},
                            "planner.tool_call_budget_exceeded",
                            "Planner exceeded the total tool-call budget"};
                }

                const auto isTerminalCall = [](const AgentToolCall& call) {
                    return call.toolName == kSubmitExecutionManifestDraftToolName
                        || call.toolName == kSubmitNoChangesNeededToolName
                        || call.toolName == kSubmitVerificationActionRequiredToolName;
                };
                const auto terminalCalls = std::count_if(
                    turn.toolCalls.begin(), turn.toolCalls.end(), isTerminalCall);
                if (terminalCalls != 0) {
                    if (terminalCalls != 1 || turn.toolCalls.size() != 1) {
                        return {false, true, false, {},
                                "planner.invalid_submission_turn",
                                "Terminal planner submission must be the only tool call "
                                "in its turn"};
                    }
                    const auto& terminalCall = turn.toolCalls.front();
                    const bool noChanges =
                        terminalCall.toolName == kSubmitNoChangesNeededToolName;
                    const bool actionRequired =
                        terminalCall.toolName == kSubmitVerificationActionRequiredToolName;
                    AttemptResult result {true, false, false, terminalCall.argumentsJson, {}, {},
                        noChanges, attemptSuccessfulCalls, attemptInspectedPaths};
                    result.actionRequiredSubmission = actionRequired;
                    return result;
                }

                std::vector<AgentToolResult> results;
                results.reserve(turn.toolCalls.size());
                std::size_t batchReturnedBytes = 0;
                for (const auto& call : turn.toolCalls) {
                    if (!inspectTools.contains(call.toolName)) {
                        return {false, false, false, {},
                                "planner.unadvertised_tool",
                                "Planner requested a tool outside its inspect-only catalog"};
                    }
                    auto executed = toolExecution_.executeToolCall(
                        generateUuid(), auditSession.sessionId, request.runId,
                        call, request.inspectPolicy);
                    if (!executed.success()) {
                        return {false, false, false, {},
                                executed.error()->code, executed.error()->message};
                    }
                    if (executed.value().success) {
                        ++successfulCalls;
                        ++attemptSuccessfulCalls;
                        if (const auto inspectedPath =
                                extractInspectedPathFromArguments(call.argumentsJson);
                            inspectedPath.has_value()) {
                            attemptInspectedPaths.insert(*inspectedPath);
                        }
                    }
                    totalBytes += executed.value().returnedBytes;
                    batchReturnedBytes += executed.value().returnedBytes;
                    results.push_back(std::move(executed.value()));
                    ++executedCalls;
                }
                if (request.providerBudgetGuard) {
                    const auto recorded = request.providerBudgetGuard->recordToolActivity(
                        0, batchReturnedBytes);
                    if (!recorded.success()) {
                        return {false, false, false, {},
                                recorded.error()->code, recorded.error()->message};
                    }
                    const auto allowed = request.providerBudgetGuard->beginProviderTurn(stopToken);
                    if (!allowed.success()) {
                        return {false, false, false, {},
                                allowed.error()->code, allowed.error()->message};
                    }
                }
                // Package 1/2: once the cumulative Planner turn count (across
                // the primary attempt and every recovery/repair attempt in
                // this same plan() call) reaches the Backend-owned
                // inspectionBoundaryTurn, every subsequent submitted turn is
                // forced to a terminal-only NextTurnToolPolicy - never reset
                // once crossed, so a later recovery/repair attempt cannot
                // reopen unrestricted inspection merely by starting a fresh
                // session.
                std::optional<providers::NextTurnToolPolicy> forcedPolicy;
                if (turnBudgetPartition
                        && persistedTurns >= turnBudgetPartition->inspectionBoundaryTurn) {
                    providers::NextTurnToolPolicy policy;
                    policy.allowedToolNames = forcedTerminalToolNames();
                    policy.disallowParallelToolCalls = true;
                    forcedPolicy = std::move(policy);
                }
                pendingTerminalPolicy = forcedPolicy;
                turnResult = providerSession.submitToolResults(
                    std::move(results), stopToken, forcedPolicy);
                if (!turnResult.success() && request.providerBudgetGuard) {
                    request.providerBudgetGuard->abandonProviderTurn();
                }
                continue;
            }

            if (turn.finishReason == providers::ProviderAgentFinishReason::Stop) {
                return {false, true, false, {},
                        "planner.missing_submission",
                        request.allowEvidenceBackedNoChanges
                            ? "Planner stopped without calling submit_execution_manifest_draft or "
                              "submit_no_changes_needed"
                            : "Planner stopped without calling submit_execution_manifest_draft"};
            }
            if (turn.finishReason == providers::ProviderAgentFinishReason::MaxTurns) {
                return {false, false, false, {},
                        "provider.max_turns",
                        "Planner did not conclude within the provider turn budget"};
            }
            if (turn.finishReason == providers::ProviderAgentFinishReason::MaxOutputTokens) {
                return {false, false, false, {},
                        "provider.max_output_tokens",
                        "Planner response reached its output-token limit"};
            }
            if (turn.finishReason == providers::ProviderAgentFinishReason::Error) {
                return {false, false, false, {},
                        turn.errorCode.empty() ? "provider.request_failed" : turn.errorCode,
                        turn.errorMessage};
            }
            return {false, false, true, {}, {}, "Planning cancelled"};
        }
    };

    // AG-18.3 hardening (blocker 2): the immutable Run snapshot's own
    // classification, computed once and reused by every check below - never
    // rediscovered or re-derived from a Planner claim.
    const bool verificationSetupRequired = request.verificationEnvironment.has_value()
        && request.verificationEnvironment->state == VerificationEnvironmentState::SetupRequired;

    // The last successfully parsed typed draft is retained only in memory for
    // the duration of this plan() call. Package 3 compares it with the next
    // validation repair without persisting either provider submission.
    std::optional<ExecutionManifestDraft> evaluatedDraft;
    // Set only while the one validation repair is evaluated: the rejected primary draft and its
    // findings, so the repair's non-authoritative numeric controls can be restored before the
    // monotonic guard compares the drafts (ExecutionManifestRepairControls.hpp).
    struct RepairBaseline {
        ExecutionManifestDraft draft;
        std::vector<ExecutionManifestFinding> findings;
    };
    std::optional<RepairBaseline> repairBaseline;

    auto evaluateSubmission = [&](const AttemptResult& attempt)
        -> common::Result<PlanningCandidate> {
        // The Backend normalization audit lines describe the latest evaluated submission only.
        outcome.normalizations.clear();
        evaluatedDraft.reset();
        if (!attempt.submitted) {
            return common::Result<PlanningCandidate>::failure(
                {attempt.errorCode, attempt.errorMessage});
        }
        if (attempt.actionRequiredSubmission) {
            // The Backend accepts this disposition only when the Run's own
            // snapshot is genuinely SetupRequired - never inferred, never a
            // fallback for provider/schema/cancellation trouble, and never
            // accepted merely because the Planner claims it.
            if (!verificationSetupRequired) {
                return common::Result<PlanningCandidate>::failure(
                    {"planner.verification_action_required_not_applicable",
                     "submit_verification_action_required was called but the verification "
                     "environment snapshot is not SetupRequired"});
            }
            auto parsed = parseVerificationActionRequiredSubmission(attempt.submission);
            if (!parsed.success()) {
                return common::Result<PlanningCandidate>::failure(
                    {parsed.error()->code, parsed.error()->message});
            }
            PlanningCandidate candidate;
            candidate.disposition = PlanningDisposition::VerificationActionRequired;
            candidate.verificationActionRequired = std::move(parsed.value());
            return common::Result<PlanningCandidate>::ok(std::move(candidate));
        }
        if (attempt.noChangesSubmission) {
            if (!request.allowEvidenceBackedNoChanges) {
                return common::Result<PlanningCandidate>::failure(
                    {"planner.no_changes_disallowed",
                     "This Run requires an Execution Manifest; evidence-backed no changes "
                     "was not enabled by the caller"});
            }
            // AG-18.3 hardening (blocker 2): a no-op can never disguise
            // missing verification setup - when SetupRequired, the Planner
            // must either form a valid bootstrap Manifest or explicitly
            // submit_verification_action_required.
            if (verificationSetupRequired) {
                return common::Result<PlanningCandidate>::failure(
                    {"planner.verification_setup_required",
                     "No usable verification profile is available; submit a valid same-Run "
                     "bootstrap Manifest or call submit_verification_action_required instead "
                     "of a no-changes-needed disposition"});
            }
            // Attempt-scoped, not the outer cumulative successfulCalls: a
            // repair session must earn its own no-op evidence and can never
            // borrow the primary attempt's (or vice versa).
            if (attempt.attemptSuccessfulInspectionCalls == 0) {
                return common::Result<PlanningCandidate>::failure(
                    {"planner.no_changes_missing_evidence",
                     "No-changes-needed submission requires at least one successful "
                     "inspection tool call in this attempt session"});
            }
            auto parsed = parseNoChangesSubmission(attempt.submission, request.inspectPolicy);
            if (!parsed.success()) {
                return common::Result<PlanningCandidate>::failure(
                    {parsed.error()->code, parsed.error()->message});
            }
            for (const auto& item : parsed.value().evidence) {
                if (!attempt.attemptInspectedPaths.contains(
                        normalizeComparablePath(item.path))) {
                    return common::Result<PlanningCandidate>::failure(
                        {"planner.no_changes_uninspected_evidence_path",
                         "No-changes-needed evidence path was not actually inspected by "
                         "a successful tool call in this attempt session"});
                }
            }
            PlanningCandidate candidate;
            candidate.disposition = PlanningDisposition::NoChangesNeeded;
            candidate.noChanges = std::move(parsed.value());
            return common::Result<PlanningCandidate>::ok(std::move(candidate));
        }
        // MW-P3A package 2: the provider submits only the smaller typed
        // draft; the Backend compiler alone decides owned_write versus
        // planned_create (from the frozen baseline) and which read entries
        // derive a dependency edge (baselineRequiredReadPaths never does).
        // A draft parse failure is a pure shape/safety rejection - there is
        // no partially-compiled manifest worth building on top of it.
        auto draft = ExecutionManifestDraftParser::parse(attempt.submission);
        if (!draft.success()) {
            outcome.findings = {{
                draft.error()->code, draft.error()->message, std::nullopt}};
            return common::Result<PlanningCandidate>::failure(
                {draft.error()->code, draft.error()->message});
        }
        if (repairBaseline.has_value()) {
            for (auto& restored : restoreNonAuthoritativeRepairControls(
                     repairBaseline->draft, draft.value(), repairBaseline->findings)) {
                outcome.normalizations.push_back(std::move(restored));
            }
        }
        evaluatedDraft = draft.value();
        auto compiled = ExecutionManifestDraftCompiler::compile(
            {std::move(draft.value()), request.runId,
             request.baselineSnapshot.value_or(WorkspaceBaselineSnapshot{})});
        common::Result<ExecutionManifest> parsed =
            common::Result<ExecutionManifest>::ok(std::move(compiled.manifest));
        // AG-17.1: stamp the trusted routing record before validation, so a task's
        // assignee is judged against it (and the frozen Manifest carries it). The
        // provider can never author or alter it.
        parsed.value().routing = request.trustedRouting;
        // AG-18.3 hardening (blocker 2): when SetupRequired, an ordinary
        // Manifest is only ever a legitimate terminal outcome when it
        // declares a same-Run bootstrap - including one that would
        // otherwise need no environment-dependent verification at all
        // (inspect_paths-only). Silently allowing such a Manifest through
        // is exactly the "avoid ActionRequired via inspect_paths" bypass
        // this hardening closes; ManifestBootstrapValidator (run inside
        // manifests_.validate() below) still owns whether a PRESENT
        // declaration is itself well-formed.
        if (verificationSetupRequired && !parsed.value().bootstrapDeclaration.has_value()) {
            return common::Result<PlanningCandidate>::failure(
                {"planner.verification_setup_required",
                 "No usable verification profile is available; submit a valid same-Run "
                 "bootstrap Manifest (bootstrapDeclaration) or call "
                 "submit_verification_action_required instead"});
        }
        // Strict provider JSON-schema validation cannot state uniqueness
        // across sibling arrays. Canonicalize only redundant active roles;
        // the validator below remains authoritative for every unsafe or
        // contradictory combination and for all direct non-Planner callers.
        canonicalizeRedundantActiveScopeRoles(parsed.value());
        // After the active roles are canonical: a path the task still reads loses its own
        // excludedPaths note (Run de6d81fb); write/deferred/cross-task conflicts are left alone.
        for (auto& removed : canonicalizeExcludedReadConflicts(parsed.value())) {
            outcome.normalizations.push_back(std::move(removed));
        }
        canonicalizeRequiredReadDependencies(parsed.value());
        if (auto lowered = clampRequestedParallelismToCeiling(
                parsed.value(), request.trustedManifestPolicy)) {
            outcome.normalizations.push_back(std::move(*lowered));
        }
        for (auto& renamed : canonicalizeCrossTaskRequirementIds(parsed.value())) {
            outcome.normalizations.push_back(std::move(renamed));
        }
        outcome.findings = manifests_.validate(
            parsed.value(), request.trustedManifestPolicy);
        outcome.findings.insert(outcome.findings.end(),
            std::make_move_iterator(compiled.findings.begin()),
            std::make_move_iterator(compiled.findings.end()));
        if (request.baselineSnapshot.has_value()) {
            auto baselineFindings =
                RunWorkspaceScopeService::validateManifestAgainstBaseline(
                    parsed.value(), *request.baselineSnapshot);
            outcome.findings.insert(outcome.findings.end(),
                std::make_move_iterator(baselineFindings.begin()),
                std::make_move_iterator(baselineFindings.end()));
        }
        // manifest.missing_acceptance_contract no longer applies to this
        // pipeline: a compiled workTasks entry's acceptanceContract is
        // always populated from the draft's own non-optional field, and a
        // compiled coordinationTasks entry is always requestedCapability
        // None, which the check below would have exempted anyway. See
        // MW-P3A package 1's closure note (Run
        // 3ffb52df-98ca-472f-9b1c-537ab134cf18).
        // Semantic relations assume the task/path/acceptance structures are
        // otherwise valid. Reporting them on top of a malformed graph creates
        // misleading secondary findings and asks one bounded repair to solve a
        // contract it never successfully expressed in the first place.
        if (outcome.findings.empty()) {
            auto semanticFindings = plannerSemanticFindings(
                parsed.value(), request.allowEvidenceBackedNoChanges,
                request.trustedManifestPolicy.maximumCapability);
            outcome.findings.insert(outcome.findings.end(),
                std::make_move_iterator(semanticFindings.begin()),
                std::make_move_iterator(semanticFindings.end()));
        }
        if (!outcome.findings.empty()) {
            return common::Result<PlanningCandidate>::failure(
                {outcome.findings.front().code, outcome.findings.front().message});
        }
        // AG-18.2: the Backend-owned binder resolves every requirement's
        // provider-submitted profile/build-profile/target selections into
        // the exact, immutable verificationArguments the rest of the
        // verification pipeline already consumes unchanged - strictly
        // after structural/semantic validation above (so it only ever
        // binds an otherwise-valid graph) and strictly before this
        // Manifest is ever eligible for manifests_.freeze(). A binding
        // failure participates in the exact same repair mechanism as any
        // other rejected candidate below - no new control-flow path.
        auto& manifestDraft = parsed.value();
        // AG-17.0: the Backend-owned mandatory verification policy derives
        // and idempotently merges the requirements a Manifest's own write
        // scope requires (qml_lint, and cmake_build as its prerequisite),
        // from the frozen verification environment snapshot - strictly after
        // validation above and strictly before binding below, so an injected
        // requirement is only a draft selection that the one existing binder
        // resolves exactly like a Planner-declared one. It is skipped when no
        // snapshot exists (nothing to derive from). A failure is a typed
        // verification_policy.* rejection that is NOT sent to the paid repair
        // (isNonRepairableSubmissionFailure).
        if (request.verificationEnvironment.has_value()) {
            const auto mandatory = ManifestMandatoryVerificationPolicy::apply(
                manifestDraft, *request.verificationEnvironment, outcome.findings);
            if (!mandatory.success()) {
                return common::Result<PlanningCandidate>::failure(
                    {mandatory.error()->code, mandatory.error()->message});
            }
        }
        const VerificationEnvironmentSnapshot emptySnapshot;
        const auto bound = ManifestVerificationProfileBinder::bindManifest(
            manifestDraft, request.verificationEnvironment.value_or(emptySnapshot),
            outcome.findings);
        if (!bound.success()) {
            return common::Result<PlanningCandidate>::failure(
                {bound.error()->code, bound.error()->message});
        }
        // worker_capacity_reservation_v3 (each task's floor reserved for its first attempt AND
        // for its protected retry session): whether the Run can afford this Manifest is a
        // deterministic fact of the finished, verification-bound task set, so it is judged
        // HERE - with the very authority the post-freeze admission uses - before the answer
        // the Run paid for is accepted and frozen. A refusal is repairable feedback (re-shape
        // the work), not a frozen Manifest the Backend then declines to execute. Skipped when
        // no shared guard exists (direct unit callers).
        if (request.providerBudgetGuard) {
            auto capacityFindings = manifestCapacityFindings(
                manifestDraft, request.providerBudgetGuard->policy(),
                request.providerBudgetGuard->usage());
            if (!capacityFindings.empty()) {
                outcome.findings.insert(outcome.findings.end(),
                    std::make_move_iterator(capacityFindings.begin()),
                    std::make_move_iterator(capacityFindings.end()));
                return common::Result<PlanningCandidate>::failure(
                    {outcome.findings.front().code, outcome.findings.front().message});
            }
        }
        PlanningCandidate candidate;
        candidate.disposition = PlanningDisposition::ManifestReady;
        candidate.manifest = std::move(manifestDraft);
        return common::Result<PlanningCandidate>::ok(std::move(candidate));
    };

    auto firstSession = std::move(firstSessionResult.value());
    auto attempt = runAttempt(*firstSession, sessionRequest);
    if (attempt.cancelled || (!attempt.submitted && !attempt.rejected)) {
        finishAudit(AgentToolSessionState::Interrupted);
        outcome.cancelled = attempt.cancelled;
        outcome.errorCode = attempt.errorCode;
        outcome.errorMessage = attempt.errorMessage;
        fillEvidence();
        return outcome;
    }

    auto candidate = evaluateSubmission(attempt);
    if (!candidate.success()) {
        outcome.primaryRejectionCode = candidate.error()->code;
        outcome.lastRejectionCode = candidate.error()->code;
        outcome.primaryFindings = outcome.findings;
        outcome.lastFindings = outcome.findings;
    }

    // Missing a terminal action is a protocol/terminalization failure, not a
    // malformed submitted graph. Give it one independently-bounded recovery
    // without consuming the one validation repair. This closes the real-run
    // chain where primary Stop used the sole old repair and that repair's
    // invalid Manifest could no longer receive its own findings. Both budgets
    // are finite and every recovered submission still passes the exact same
    // parser, validator, and no-op evidence gates above.
    bool terminalizationRecoveryUsed = false;
    bool validationRepairUsed = false;
    while (!candidate.success()) {
        // AG-17.0: a deterministic Backend policy decision cannot be fixed by
        // a second provider turn - end planning before any repair session is
        // created, so the provider was called exactly once.
        if (isNonRepairableSubmissionFailure(candidate.error()->code)) break;
        const bool terminalizationRecovery =
            candidate.error()->code == "planner.missing_submission";
        if (terminalizationRecovery) {
            if (terminalizationRecoveryUsed) break;
            terminalizationRecoveryUsed = true;
        } else {
            if (validationRepairUsed) break;
            validationRepairUsed = true;
        }

        // Capture the rejected typed draft immediately before its one
        // validation-repair session. A terminalization recovery has no typed
        // submission to constrain; if that recovery produces an invalid
        // draft, the following validation iteration captures it here.
        const auto monotonicBaseDraft = terminalizationRecovery
            ? std::optional<ExecutionManifestDraft> {}
            : evaluatedDraft;
        const auto monotonicBaseFindings = outcome.findings;

        const std::string rejectionCode = candidate.error()->code;
        const std::string rejectionMessage = candidate.error()->message;
        const auto rejectionFindings = outcome.findings;
        // Real-Run incident (Runs fcd484b8-.../cebcd3cf-.../c5f7d3da-...,
        // 2026-09-11): the specific correction-instruction blocks below used
        // to gate only on rejectionCode (== rejectionFindings.front().code) -
        // when a primary submission carried SEVERAL different finding types
        // at once (e.g. manifest.inspect_task_command_verification and a
        // manifest.dependency_cycle together), only whichever one happened
        // to sort first ever got a specific instruction; every other finding
        // type was left with only its raw code/message in the generic
        // "Rejection findings" list below, with no correction guidance at
        // all. A repair session then fixed only what it was actually told
        // how to fix, silently leaving the rest unresolved (and, for a
        // cycle, submitting the same or an equally cyclic graph again).
        // Every block below now checks the WHOLE findings set, so every
        // applicable specific instruction is always included regardless of
        // finding order - rejectionCode/rejectionMessage remain the
        // headline (the first-reported finding), but never gate which
        // corrections the model actually receives.
        const auto hasRejectionCode = [&](std::string_view code) {
            return std::any_of(rejectionFindings.begin(), rejectionFindings.end(),
                [&](const ExecutionManifestFinding& finding) { return finding.code == code; });
        };
        const auto hasRejectionCodePrefix = [&](std::string_view prefix) {
            return std::any_of(rejectionFindings.begin(), rejectionFindings.end(),
                [&](const ExecutionManifestFinding& finding) {
                    return finding.code.starts_with(prefix);
                });
        };
        ++outcome.repairAttempts;

        auto repairSessionResult = sessionFactory_.createSession(
            sessionRequest.providerId, request.providerKind);
        if (!repairSessionResult.success()) {
            finishAudit(AgentToolSessionState::Interrupted);
            outcome.errorCode = repairSessionResult.error()->code;
            outcome.errorMessage = repairSessionResult.error()->message;
            fillEvidence();
            return outcome;
        }

        providers::ProviderAgentSessionRequest repairRequest = sessionRequest;
        // Real-Run incident (Run a739e790-db80-4efd-a98f-5d91af2e5e86,
        // 2026-09-14): a session that legitimately batches many parallel
        // tool calls into one turn can exceed maxToolCallsPerTurn and hard-
        // fail before any useful work. The one bounded repair session is a
        // structural guarantee, not just a bigger ceiling: forcing at most
        // one tool call per turn here means this specific wall can never be
        // hit again on the repair attempt, regardless of how many files the
        // Planner wants to (re-)inspect - the primary attempt keeps the
        // normal higher per-turn ceiling for its own efficiency.
        repairRequest.disallowParallelToolCalls = true;
        std::ostringstream repair;
        repair << sessionRequest.userContent;
        if (terminalizationRecovery) {
            repair << "\n\n# One bounded terminalization recovery\n"
                   << "The previous fresh session ended without either required "
                      "terminal tool (" << rejectionCode << ": " << rejectionMessage
                   << "). This recovery exists only to finish the CURRENT Run task's "
                      "planning protocol. Reinspect the workspace in this fresh session "
                      "as needed, perform the completion preflight, then call exactly "
                      "one standalone terminal tool. ";
            if (request.allowEvidenceBackedNoChanges) {
                repair << "If no concrete downstream work is missing, call "
                          "submit_no_changes_needed with same-session inspected evidence. ";
            } else {
                repair << "This Run requires submit_execution_manifest_draft; "
                          "submit_no_changes_needed is disallowed. ";
            }
            repair << "Do not create repository-audit, inspection-only, "
                      "verify-and-report, or broad root-ownership tasks merely to produce "
                      "a non-empty graph.";
        } else {
            repair << "\n\n# One bounded validation repair\n"
                   << "The previous terminal submission was rejected with "
                   << rejectionCode << ": " << rejectionMessage
                   << ". Correct the rejected shape or graph constraints. Submit a "
                      "Manifest only for concrete remaining execution work, with clean "
                      "forward-slash workspace-relative writePaths that are neither root, "
                      "absolute, dotted, globbed, nor equal/ancestor/descendant overlaps. "
                      "This repair is monotonic: when the findings name concrete task ids, "
                      "change only those tasks and preserve every unrelated task and all "
                      "top-level controls exactly. The Backend rejects the repair as "
                      "planner.repair_regressed if it rewrites unrelated allocation or "
                      "trades the reported finding for a new one.";
            // A shape/field rejection (manifest.*/acceptance.*) gets an
            // explicit schema restatement here, not just the system prompt's
            // own wording: a repair session that already produced one
            // incomplete submission is exactly the case where re-reading a
            // shortened mental model of the schema (e.g. only
            // id/requiredOutputs/verificationKind) is most likely to repeat
            // the same omission, so this restates the full field list a
            // second time, at the point of known failure.
            if (hasRejectionCodePrefix("manifest.") || hasRejectionCodePrefix("acceptance.")) {
                repair << " The top-level submission itself always contains exactly these "
                          "seven fields, none omitted: schemaVersion, coordinationTasks, "
                          "workTasks, "
                          "requestedMaxParallelism, requestedTokenBudget (integer or null), "
                          "requestedCostMicros (integer or null), bootstrapDeclaration (an "
                          "object only when declaring a same-Run verification bootstrap, "
                          "otherwise the JSON literal null). A coordinationTasks entry always "
                          "contains exactly these four fields, none omitted: id, objective, "
                          "role, dependencies - never a path/read/acceptance field of any kind. "
                          "A workTasks entry always "
                          "contains exactly these thirteen fields, none omitted: id, objective, "
                          "role, dependencies, writePaths, "
                          "baselineRequiredReadPaths, proposedDependencyReadPaths, "
                          "referencePaths, excludedPaths, deferredPaths, "
                          "acceptanceCriteria, "
                          "acceptanceContract (always a real object - never the JSON literal "
                          "null, and never omitted; a task with no real work belongs in "
                          "coordinationTasks instead), contextReferences, "
                          "requestedCapability (inspect or propose only). All five array-typed "
                          "path-role fields must be present. writePaths, "
                          "baselineRequiredReadPaths, proposedDependencyReadPaths, and "
                          "referencePaths contain path strings. excludedPaths and "
                          "deferredPaths contain only objects with exactly two string fields, "
                          "path and reason; reason must be nonblank. Use [] when a role has "
                          "no entries. "
                          "acceptanceContract always "
                          "contains exactly these two fields, none omitted: schemaVersion, "
                          "requirements. Every acceptanceContract requirement object always "
                          "contains exactly these ten fields, none omitted: id, description, "
                          "requiredOutputs, verificationKind, verificationProfileId, "
                          "verificationBuildProfileId, verificationTargetId (each a catalog id "
                          "string or the JSON literal null), verificationArguments (an array "
                          "- use [] when there are no directly-submitted arguments; never omit "
                          "this field), required (a boolean), verificationBootstrapPending (a "
                          "boolean, true only for a cmake_build/qml_lint requirement legitimately "
                          "deferred pending bootstrapDeclaration). Every non-empty "
                          "verificationArguments item always contains exactly these two "
                          "fields, none omitted: name, value. Re-check every top-level field, "
                          "every task, every acceptanceContract object, every requirement "
                          "object, and every verificationArguments item against these exact "
                          "field lists before submitting again. Selections admit no extra or "
                          "invented ids: inspect_paths requires verificationProfileId, "
                          "verificationBuildProfileId, and verificationTargetId all null and "
                          "verificationArguments []; cmake_build requires a real catalog "
                          "verificationProfileId and its linked verificationBuildProfileId "
                          "(verificationTargetId null, verificationArguments []); qml_lint "
                          "requires all three catalog ids (verificationArguments []). There is "
                          "no cmake_target_consistency kind available to a submission - never "
                          "use it; represent ordinary CMake/QML acceptance with cmake_build "
                          "and/or qml_lint instead. Never place preset, "
                          "buildPreset, buildDirectory, or a raw target name directly in "
                          "verificationArguments for any kind - those come exclusively from "
                          "the selection ids and the trusted catalog resolves them. Represent "
                          "build and QML lint as separate requirements when both are needed, "
                          "and select only ids literally advertised in the trusted "
                          "verification environment catalog supplied in this message. Every "
                          "Manifest containing qml_lint must also contain a required "
                          "cmake_build requirement because candidate verification never "
                          "reuses the workspace's existing build directory.";
            }
            if (!assignableMemberIds.empty()) {
                // The thirteen-field restatement above would otherwise drop the
                // roster field from the repair session's mental model of a task.
                repair << " This is a roster Run: every workTasks entry ALSO contains "
                          "assigneeMemberId (fourteen fields in total, none omitted), set to "
                          "exactly one of these assignable memberIds copied verbatim: ";
                for (std::size_t index = 0; index < assignableMemberIds.size(); ++index) {
                    repair << (index == 0 ? "" : ", ") << "\"" << assignableMemberIds[index] << "\"";
                }
                repair << ". Never the lead Architect and never an id outside that list; a "
                          "coordinationTasks entry has no assignee field at all.";
            }
            if (hasRejectionCodePrefix("manifest.verification_")
                || hasRejectionCode("acceptance.invalid_verification_selection")
                || hasRejectionCode("acceptance.provider_owned_verification_argument")) {
                repair << " This rejection concerns a verification profile/build-profile/"
                          "target selection. Select ONLY ids literally advertised in the "
                          "trusted verification environment catalog supplied in this message - "
                          "never invent, guess, or recombine an id, and never fall back to a "
                          "raw preset/buildPreset/buildDirectory/target value in "
                          "verificationArguments. If the catalog offers no profile whose "
                          "buildPresetsConfirmed (and, for qml_lint, allowedTargetsConfirmed) "
                          "is true for THIS specific requirement's task, that capability is not "
                          "currently available for that task - either use inspect_paths for "
                          "that specific requirement (never for the whole Manifest merely to "
                          "avoid the verification lifecycle), or defer it with "
                          "verificationBootstrapPending against a valid bootstrapDeclaration if "
                          "this Run can safely create the missing setup.";
            }
            if (hasRejectionCode("acceptance.untrusted_cmake_consistency_selection")) {
                repair << " cmake_target_consistency was rejected: the Backend has no trusted "
                          "catalog of admissible definition/registration/target triples yet, so "
                          "this kind is never accepted from a submission. Replace that "
                          "requirement with cmake_build (and, if you need to verify QML "
                          "specifically, also qml_lint) against a real catalog profile from the "
                          "trusted verification environment catalog. Never invent a literal "
                          "source-file substring check of your own, and never substitute a "
                          "generated build-system target name (for example one ending in "
                          "_qmllint) into any kind of consistency check against your own source "
                          "files.";
            }
            if (hasRejectionCode("planner.verification_setup_required")) {
                repair << " No usable verification profile is currently available for this "
                          "Run. You have exactly two safe options: submit_execution_manifest_draft "
                          "with a valid same-Run bootstrapDeclaration (see the bootstrap rules "
                          "above) if this Run is authorized to create or modify the missing "
                          "CMakePresets.json/CMakeUserPresets.json, or otherwise call "
                          "submit_verification_action_required with a bounded summary/reason "
                          "explaining why no safe bootstrap could be formed. Do not resubmit an "
                          "ordinary Manifest without bootstrapDeclaration, and do not call "
                          "submit_no_changes_needed - neither is accepted in this state.";
            }
            if (hasRejectionCode("planner.verification_action_required_not_applicable")) {
                repair << " submit_verification_action_required is only accepted when this "
                          "Run's verification environment genuinely has no usable profile. An "
                          "existing profile is available - select a catalog id from the trusted "
                          "verification environment catalog instead, using "
                          "submit_execution_manifest_draft.";
            }
            if (hasRejectionCodePrefix("verification_environment.bootstrap_")
                || hasRejectionCodePrefix("manifest.bootstrap_")) {
                repair << " This rejection concerns the same-Run verification bootstrap. "
                          "bootstrapDeclaration.setupPaths must be exactly CMakePresets.json "
                          "and/or CMakeUserPresets.json at the repository root - never a "
                          "subdirectory or other filename. Each declared setup file must "
                          "appear exactly once in the whole Manifest: in "
                          "bootstrapTaskId's own writePaths (the Backend classifies "
                          "existing-versus-new from the baseline, never split this yourself) - "
                          "never in any "
                          "path-role list of any other task, and never in excludedPaths, "
                          "deferredPaths, referencePaths, baselineRequiredReadPaths, or "
                          "proposedDependencyReadPaths of the bootstrap "
                          "task itself. Every other task that sets verificationBootstrapPending "
                          "true on one of its requirements must list bootstrapTaskId in its own "
                          "dependencies. If a safe bootstrap genuinely cannot be formed this "
                          "way, call submit_verification_action_required instead - never remove "
                          "bootstrapDeclaration and resubmit an ordinary inspect_paths-only "
                          "Manifest merely to avoid this rejection.";
            }
            if (hasRejectionCode("manifest.conflicting_path_role")) {
                // Real-Run incident (Runs 37640091-.../207df296-..., 2026-09-14,
                // real Stage 2B on OpenAI): both real Runs' PRIMARY rejection
                // was manifest.none_task_not_empty or manifest.conflicting_path_role
                // - never manifest.dependency_cycle - so the dedicated cycle
                // guidance below never fired for the repair prompt that was
                // actually sent. The repair session then reassigned a path's
                // role/ownership to satisfy THIS check and, in doing so,
                // created a fresh cross-task cycle the FINAL validation
                // caught - by then the one bounded repair was already spent.
                // Reassigning which task owns/reads a path is exactly the
                // kind of change that can create or reveal a cycle, so this
                // caution is now unconditional here, not only inside the
                // dependency_cycle block below.
                repair << " For manifest.conflicting_path_role, rebuild every task's six "
                          "path-role lists (writePaths, baselineRequiredReadPaths, "
                          "proposedDependencyReadPaths, referencePaths, excludedPaths, "
                          "deferredPaths) so each normalized path appears in exactly one "
                          "list. Keep any file the task writes - existing or new - only in "
                          "writePaths; the worker can already read its own writePaths, so remove "
                          "that path from either read list and referencePaths. Use "
                          "baselineRequiredReadPaths only for the current content of a "
                          "pre-existing path you do not depend on another task to produce, "
                          "proposedDependencyReadPaths only for a path another task in this "
                          "same submission is genuinely producing, referencePaths only for "
                          "optional "
                          "read-only context, and never repeat excluded/deferred paths in an "
                          "active role. Moving a path into a different role can change which "
                          "task owns it, which can create a NEW dependency cycle even though "
                          "none was reported this time - after reassigning any path's role, "
                          "recheck whether any two tasks now each require a path the OTHER "
                          "one produces; if so, merge those two tasks into one rather than "
                          "resubmitting a graph that is cyclic in a new way.";
                // Real-Run incident (Run ed9b84fc-f0fd-4796-b7d2-743715247be1,
                // 2026-09-27, real Anthropic canary): the primary conflict was a
                // single referencePaths/excludedPaths pair on one path
                // (CMakeLists.txt). The one bounded repair did not remove it -
                // it kept that same conflict AND reproduced it on every other
                // untouched baseline file, turning 1 conflicting path into 6
                // and exhausting the repair budget on a worse manifest. The
                // generic six-list guidance above was not enough; this
                // specific confusion needs its own explicit, unmissable rule.
                repair << " IMPORTANT, specifically for a referencePaths/excludedPaths "
                          "conflict: referencePaths means allowed, read-only context you may "
                          "consult; excludedPaths means a path deliberately kept OUT of every "
                          "active role (no read, no write, no reference) for a genuine, "
                          "stated reason. The same normalized path must never appear in both "
                          "lists at once. Do not put every untouched baseline file into "
                          "excludedPaths just because this task will not modify it - a file "
                          "this task never needs for any reason belongs in NEITHER list at "
                          "all. If this task actually reads a file for context, it belongs in "
                          "referencePaths (or baselineRequiredReadPaths/"
                          "proposedDependencyReadPaths when that read is required, not "
                          "optional) - never excludedPaths. A build/module-registration file "
                          "such as qml/CMakeLists.txt that helps this task's own written file "
                          "resolve correctly belongs in referencePaths, not excludedPaths. Fix "
                          "this by removing the exact conflicting path this finding names from "
                          "whichever one of the two lists is wrong for it. It is NEVER an "
                          "acceptable fix to resolve one reported referencePaths/excludedPaths "
                          "conflict by adding the rest of the baseline into both "
                          "referencePaths and excludedPaths at the same time - that reproduces "
                          "the exact same defect on every other file instead of fixing it.";
            }
            if (hasRejectionCode("manifest.none_task_not_empty")) {
                // Real-Run incident (Run 37640091-..., 2026-09-14): no
                // dedicated guidance existed for this code at all - the
                // repair session received only the raw finding text and had
                // to guess. It guessed by giving the task real paths without
                // declaring the resulting cross-task dependency, producing
                // manifest.dependency_cycle on the one bounded repair.
                repair << " For manifest.none_task_not_empty, a None-capability task must have "
                          "no ownedPaths, no acceptanceCriteria, and no acceptanceContract "
                          "requirements - it is a pure coordination node with no file work of "
                          "its own. Fix this one of two ways: (1) if this task genuinely has no "
                          "file work, clear its ownedPaths/acceptanceCriteria/acceptanceContract "
                          "entirely and leave requestedCapability as None; or (2) if it does have "
                          "real file work (the task id and objective usually make this obvious), "
                          "change requestedCapability to Inspect or Propose, matching whatever "
                          "capability its real work needs - never leave it None while giving it "
                          "paths or criteria. If choosing (2) adds or keeps a path another task "
                          "also needs to read or write, declare that dependency correctly (see "
                          "manifest.missing_scope_dependency below) and confirm the result is "
                          "still acyclic - if two tasks would each require a path the OTHER "
                          "produces, they are not separable: merge them into one task instead.";
            }
            if (hasRejectionCode("manifest.missing_scope_dependency")) {
                // The finding message already names the exact consumer task,
                // producer task, and normalized workspace-relative path (see
                // RunWorkspaceScopeService::validateManifestAgainstBaseline) -
                // this restates the general correction rule so the repair
                // session does not have to infer it, and explicitly forbids
                // the two most likely wrong "fixes": dropping the read
                // dependency, or reclassifying the path into a different
                // role merely to dodge this specific check.
                repair << " manifest.missing_scope_dependency: the rejection finding below "
                          "names the exact consumer task, the exact producer task, and the "
                          "exact normalized path. This only ever concerns a "
                          "proposedDependencyReadPaths entry (never baselineRequiredReadPaths, "
                          "which never requires a declared dependency): if a task lists a path "
                          "in proposedDependencyReadPaths "
                          "and a DIFFERENT task's writePaths claims that same path, the "
                          "consumer task must list that producer "
                          "task's id in its own dependencies. Keep the path exactly where it "
                          "is in proposedDependencyReadPaths - never remove it or move it into "
                          "referencePaths, excludedPaths, or deferredPaths merely to silence "
                          "this check. Add only the single producer task id the finding "
                          "identifies to the consumer's dependencies; do not add any other "
                          "dependency edge, and do not create a cycle - the dependency graph "
                          "must remain acyclic. Leave every other task, every owned/"
                          "planned-create path, every acceptanceContract, and every "
                          "verification selection exactly as already submitted. Resubmit one "
                          "complete Manifest with this one corrected dependency list, not a "
                          "partial patch.";
            }
            if (hasRejectionCode("manifest.inspect_task_command_verification")) {
                // The finding names the exact task and requirement. An
                // inspect-capability task's own worker session can only ever
                // self-verify by reading files and reporting evidence through
                // submit_task_verification - it never actually runs a build or
                // lint command, and the Backend independently refuses to accept
                // a command-backed requirement reported Passed that way. A
                // Manifest shaped like this fails deterministically on every
                // attempt, regardless of the real project state.
                repair << " manifest.inspect_task_command_verification: the rejection finding "
                          "below names the exact task and the exact requirement whose "
                          "verificationKind is not inspect_paths. An inspect-capability task's "
                          "worker session never runs cmake_build, qml_lint, or "
                          "cmake_target_consistency itself - it only reads files and "
                          "self-reports evidence via submit_task_verification, and the Backend "
                          "will never accept a command-backed requirement reported Passed that "
                          "way. Fix this in one of exactly two ways, whichever matches what this "
                          "requirement actually needs: (1) if real build/lint verification is "
                          "genuinely needed, move that requirement onto a propose-capability "
                          "task's acceptanceContract instead - command-backed checks are "
                          "evaluated once, automatically, across the whole Manifest by the "
                          "Backend's own combined verification after every propose-capability "
                          "task's proposal is in place, never by an individual worker session; "
                          "or (2) if this task should remain inspect-capability, replace the "
                          "offending requirement's verificationKind with inspect_paths and give "
                          "it real requiredOutputs (exact files this task will confirm by "
                          "reading them) instead of a build/lint profile selection. Never "
                          "describe an inspect-capability task as running, confirming, or "
                          "gating on a build or lint result - it cannot. Leave every other task, "
                          "path allocation, and requirement exactly as already submitted. "
                          "Resubmit one complete Manifest.";
            }
            if (hasRejectionCode("manifest.dependency_cycle")) {
                // Real-Run incident (Runs fcd484b8-.../cebcd3cf-.../
                // c5f7d3da-..., 2026-09-11): the finding now names the exact
                // cycle chain (task -> task -> ... -> the same task again -
                // see ExecutionManifestValidator's cycle detection). The
                // dominant real-world cause is a Backend-derived producer/
                // consumer edge (see manifest.missing_scope_dependency)
                // pointing back at a task that ALSO required-reads a path
                // the first task owns - i.e. two tasks each need a path the
                // OTHER one produces, which is not actually two separable
                // tasks at all.
                // Real-Run incident (Runs 37640091-.../207df296-.../
                // e8275bde-.../397c0ffb-.../2d2cf273-..., 2026-09-14): three
                // further real attempts against the SAME real task kept
                // resubmitting an unchanged or near-unchanged cyclic graph
                // even with the merge instruction below already present in
                // the primary findings - merging two tasks whose real jobs
                // are genuinely unrelated (creating new QML view files vs.
                // wiring navigation) is itself the wrong fix and a model may
                // reasonably resist doing it. In every one of these Runs one
                // of the two cycle-forming edges was a pre-existing project-
                // context/documentation file neither task's real work
                // modifies - see the false-ownership guidance above. Check
                // that first, before considering a merge.
                repair << " manifest.dependency_cycle: the finding above names the exact cycle "
                          "chain (task -> task -> ... -> the same task again) and, for each edge, "
                          "the exact path that created it. First check whether either edge-forming "
                          "path is a pre-existing project-context/documentation file that this "
                          "Run's real work does not substantively rewrite (see the guidance above "
                          "on never inventing ownership of such a file) - if so, the fix is to "
                          "remove that path from whichever task's writePaths "
                          "falsely claims it and instead list it directly in "
                          "baselineRequiredReadPaths (never proposedDependencyReadPaths) (or "
                          "referencePaths) of every task that actually needs to read it; do NOT "
                          "merge the tasks in this case, since their real work remains genuinely "
                          "separate. Only when BOTH tasks in the named cycle genuinely produce "
                          "something specific to this Run's real deliverables that the OTHER one "
                          "needs - not a shared pre-existing reference file - are they not "
                          "separable: merge them into one task that owns/plans-creates every path "
                          "either one needed to write, with a single combined objective and "
                          "acceptanceContract, so no circular producer-consumer edge can exist "
                          "between them. A dependency edge always points from the CONSUMER (the "
                          "task whose proposedDependencyReadPaths names a path) to the PRODUCER "
                          "(the task "
                          "whose writePaths claims that same path) - never the "
                          "reverse, and never added merely to acknowledge a review comment. Never "
                          "remove, reclassify, or silently drop a genuinely required "
                          "read path merely to break the cycle - that only hides a real "
                          "data dependency and will surface again. Resubmit one complete Manifest "
                          "with the cycle actually resolved - never by resubmitting the same or an "
                          "equally cyclic graph.";
            }
            if (hasRejectionCode("planner.non_executable_manifest")) {
                repair << " This is an implementation Run. Replace audit-only, "
                          "inspection-only, and report-only filler work with at least "
                          "one propose-capability task that owns exact existing files "
                          "or declares exact planned-create files for the concrete "
                          "remaining implementation. A later human Apply step remains "
                          "separate; 'do not apply automatically' does not forbid "
                          "Change Proposals.";
            }
            if (hasRejectionCode("manifest.acceptance_output_outside_task_scope")) {
                repair << " Every requiredOutputs value must name an exact file in "
                          "that same task's writePaths, baselineRequiredReadPaths, or "
                          "proposedDependencyReadPaths. Never use a report label, artifact name, "
                          "or descriptive phrase as a required output.";
            }
            if (hasRejectionCode("manifest.baseline_required_read_path_not_in_baseline")) {
                // MW-P3A package 2: the Backend compiler itself rejects this
                // - the model claimed a path under baselineRequiredReadPaths
                // (the CURRENT baseline content) that does not actually
                // exist in the frozen baseline at all.
                repair << " manifest.baseline_required_read_path_not_in_baseline: a "
                          "baselineRequiredReadPaths entry named a path that does not exist in "
                          "the Run's frozen baseline. If the path is genuinely new - only some "
                          "other task in THIS submission is going to create or write it - move "
                          "the entry to proposedDependencyReadPaths instead, naming that other "
                          "task in your own dependencies (see "
                          "manifest.proposed_dependency_read_path_has_no_producer below and "
                          "manifest.missing_scope_dependency above). If the path does not exist "
                          "and nothing in this submission creates it either, remove it entirely - "
                          "you cannot require a baseline read of a file that never existed and "
                          "nothing produces.";
            }
            if (hasRejectionCode("manifest.proposed_dependency_read_path_has_no_producer")) {
                repair << " manifest.proposed_dependency_read_path_has_no_producer: a "
                          "proposedDependencyReadPaths entry did not name any other work task's "
                          "writePaths entry in this same submission. If you only need the "
                          "path's CURRENT, already-existing content and are not actually waiting "
                          "on another task's result, move the entry to baselineRequiredReadPaths "
                          "instead (it must then exist in the frozen baseline - see "
                          "manifest.baseline_required_read_path_not_in_baseline above). If you "
                          "genuinely need another task's future output, add a work task whose "
                          "writePaths contains this exact path, or correct the path to match the "
                          "producing task's actual writePaths entry - never invent a producer "
                          "that does not exist in this submission.";
            }
            if (hasRejectionCodePrefix("planner.capacity_")) {
                // The submission itself was well-formed and fully valid; the Run cannot afford
                // it. The numbers are in the findings below, so this only explains the unit.
                repair << " planner.capacity_*: the findings below concern the Run's provider-turn "
                          "capacity, not the shape of the submission - the structure itself was "
                          "accepted. Each task's worker needs one provider turn per required read "
                          "(baselineRequiredReadPaths plus proposedDependencyReadPaths of a propose "
                          "task; the distinct requiredOutputs of an inspect task) and, for a propose "
                          "task, one per existing file it writes (a writePaths entry that already "
                          "exists in the baseline; a new file costs nothing), plus one terminal "
                          "turn, and all tasks share the Run's turn budget. Reduce the required "
                          "reads: keep only files a task must read before it proposes; move "
                          "optional context into referencePaths (those are not counted); split a "
                          "task that writes many existing files into smaller tasks, or drop "
                          "work that is not essential. When you remove a path from "
                          "a task's required reads, also remove it from that task's "
                          "requiredOutputs unless the task writes it. Keep every task the findings "
                          "do not name exactly as submitted. Resubmit one complete Manifest.";
            }
            if (request.allowEvidenceBackedNoChanges) {
                repair << " If the CURRENT task is already satisfied, you may instead call "
                          "submit_no_changes_needed, but this fresh session must first inspect "
                          "every evidence path it cites.";
            } else {
                repair << " This Run requires submit_execution_manifest_draft; "
                          "submit_no_changes_needed is not accepted.";
            }
            repair << " Call exactly one standalone terminal tool. This is the only "
                      "validation repair.";
        }
        if (!rejectionFindings.empty()) {
            repair << " Rejection findings:";
            for (const auto& finding : rejectionFindings) {
                repair << " [" << finding.code << "] " << finding.message;
                if (finding.taskId.has_value()) repair << " (task " << *finding.taskId << ")";
            }
        }
        repairRequest.userContent = repair.str();
        outcome.findings.clear();

        auto repairSession = std::move(repairSessionResult.value());
        if (monotonicBaseDraft.has_value()) {
            repairBaseline = RepairBaseline {*monotonicBaseDraft, monotonicBaseFindings};
        }
        attempt = runAttempt(*repairSession, std::move(repairRequest));
        if (attempt.cancelled || (!attempt.submitted && !attempt.rejected)) {
            finishAudit(AgentToolSessionState::Interrupted);
            outcome.cancelled = attempt.cancelled;
            outcome.errorCode = attempt.errorCode;
            outcome.errorMessage = attempt.errorMessage;
            fillEvidence();
            return outcome;
        }
        candidate = evaluateSubmission(attempt);
        // AG-17.0: when the repaired submission reaches the Backend policy and
        // is rejected there, that typed code is the outcome - never masked by
        // the repair-regression guards below, which judge the model's edit.
        if (!terminalizationRecovery
            && !(!candidate.success()
                 && isNonRepairableSubmissionFailure(candidate.error()->code))) {
            // Typed structural comparison needs two parsed drafts, but the
            // finding-identity guard does not. In particular, a parser-level
            // primary rejection such as manifest.missing_field has no
            // monotonicBaseDraft; its one bounded repair must still be
            // rejected if it merely trades that defect for an unrelated
            // validation failure.
            const auto monotonic =
                monotonicBaseDraft.has_value() && evaluatedDraft.has_value()
                ? validateMonotonicDraftChange(
                    *monotonicBaseDraft, *evaluatedDraft, monotonicBaseFindings)
                : common::Result<void>::ok();
            const bool newFinding = !candidate.success()
                && introducesUnrelatedFinding(monotonicBaseFindings, rejectionCode,
                    outcome.findings, candidate.error()->code);
            if (!monotonic.success() || newFinding) {
                const std::string message = !monotonic.success()
                    ? monotonic.error()->message
                    : "Planner repair introduced a finding unrelated to the primary rejection";
                outcome.findings.insert(outcome.findings.begin(), {
                    "planner.repair_regressed", message, std::nullopt});
                candidate = common::Result<PlanningCandidate>::failure(
                    {"planner.repair_regressed", message});
            }
        }
        if (!candidate.success()) {
            outcome.lastRejectionCode = candidate.error()->code;
            outcome.lastFindings = outcome.findings;
        }
    }

    if (!candidate.success()) {
        finishAudit(AgentToolSessionState::Interrupted);
        if (isNonRepairableSubmissionFailure(candidate.error()->code)) {
            // AG-17.0: the typed Backend policy code, not a repair outcome.
            outcome.errorCode = candidate.error()->code;
            outcome.errorMessage = candidate.error()->message;
            outcome.primaryRejectionCode = outcome.primaryRejectionCode.empty()
                ? candidate.error()->code : outcome.primaryRejectionCode;
            outcome.lastRejectionCode = candidate.error()->code;
            if (outcome.primaryFindings.empty()) outcome.primaryFindings = outcome.findings;
            outcome.lastFindings = outcome.findings;
        } else {
            outcome.errorCode = "planner.repair_exhausted";
            outcome.errorMessage =
                "The planner's bounded terminalization/validation recovery budget was exhausted";
        }
        fillEvidence();
        return outcome;
    }

    if (candidate.value().disposition == PlanningDisposition::NoChangesNeeded) {
        // No manifest is frozen or persisted for a no-op: findings 6/7 of the
        // fix plan require the stored manifest to stay absent and no worker
        // session to ever be created for this disposition.
        finishAudit(AgentToolSessionState::Completed);
        outcome.success = true;
        outcome.disposition = PlanningDisposition::NoChangesNeeded;
        outcome.noChangesNeeded = std::move(candidate.value().noChanges);
        fillEvidence();
        return outcome;
    }

    if (candidate.value().disposition == PlanningDisposition::VerificationActionRequired) {
        // AG-18.3 hardening (blocker 2): no manifest is ever frozen or
        // persisted for this disposition, and no worker session was ever
        // considered - the explicit submission was accepted only because
        // the immutable snapshot is genuinely SetupRequired (checked by
        // evaluateSubmission above, before this candidate could exist).
        finishAudit(AgentToolSessionState::Completed);
        outcome.success = true;
        outcome.disposition = PlanningDisposition::VerificationActionRequired;
        outcome.verificationActionRequired = std::move(candidate.value().verificationActionRequired);
        fillEvidence();
        return outcome;
    }

    auto frozen = manifests_.freeze(
        std::move(*candidate.value().manifest), request.trustedManifestPolicy);
    if (!frozen.success()) {
        finishAudit(AgentToolSessionState::Interrupted);
        outcome.errorCode = frozen.error()->code;
        outcome.errorMessage = frozen.error()->message;
        fillEvidence();
        return outcome;
    }

    finishAudit(AgentToolSessionState::Completed);
    outcome.success = true;
    outcome.disposition = PlanningDisposition::ManifestReady;
    outcome.manifest = std::move(frozen.value());
    fillEvidence();
    return outcome;
}

std::string renderPlannerNoChangesNeededJson(
    const PlannerNoChangesNeeded& noChangesNeeded)
{
    nlohmann::json evidence = nlohmann::json::array();
    for (const auto& item : noChangesNeeded.evidence) {
        evidence.push_back(nlohmann::json {
            {"criterion", item.criterion},
            {"path", item.path},
            {"observation", item.observation}});
    }
    const nlohmann::json j {
        {"schemaVersion", noChangesNeeded.schemaVersion},
        {"summary", noChangesNeeded.summary},
        {"evidence", std::move(evidence)},
        {"recommendedNextStep", noChangesNeeded.recommendedNextStep.has_value()
            ? nlohmann::json(*noChangesNeeded.recommendedNextStep)
            : nlohmann::json(nullptr)}};
    return j.dump();
}

common::Result<PlannerNoChangesNeeded> parsePlannerNoChangesNeededJson(
    std::string_view json)
{
    const auto corrupt = [] {
        return common::Result<PlannerNoChangesNeeded>::failure(
            {"planner_no_changes.corrupt_identity",
             "Malformed no-changes-needed identity JSON"});
    };
    if (json.size() > kMaximumNoChangesSubmissionBytes) return corrupt();
    const auto parsed = nlohmann::json::parse(json, nullptr, false);
    if (parsed.is_discarded() || !parsed.is_object() || parsed.size() != 4) return corrupt();
    static const std::unordered_set<std::string> requiredFields {
        "schemaVersion", "summary", "evidence", "recommendedNextStep"};
    for (const auto& [key, unused] : parsed.items()) {
        static_cast<void>(unused);
        if (!requiredFields.contains(key)) return corrupt();
    }
    if (!parsed.contains("schemaVersion") || !parsed["schemaVersion"].is_string()
        || parsed["schemaVersion"].get<std::string>()
            != std::string(kPlannerNoChangesNeededSchemaVersion)
        || !parsed.contains("summary") || !parsed["summary"].is_string()
        || !parsed.contains("evidence") || !parsed["evidence"].is_array()
        || parsed["evidence"].empty()
        || parsed["evidence"].size() > kMaximumNoChangesEvidenceItems
        || !parsed.contains("recommendedNextStep")
        || !(parsed["recommendedNextStep"].is_null()
             || parsed["recommendedNextStep"].is_string())) {
        return corrupt();
    }

    PlannerNoChangesNeeded result;
    result.schemaVersion = std::string(kPlannerNoChangesNeededSchemaVersion);
    result.summary = parsed["summary"].get<std::string>();
    if (isBlank(result.summary) || result.summary.size() > kMaximumNoChangesStringBytes) {
        return corrupt();
    }
    result.evidence.reserve(parsed["evidence"].size());
    for (const auto& item : parsed["evidence"]) {
        if (!item.is_object() || item.size() != 3
            || !item.contains("criterion") || !item["criterion"].is_string()
            || !item.contains("path") || !item["path"].is_string()
            || !item.contains("observation") || !item["observation"].is_string()) {
            return corrupt();
        }
        for (const auto& [key, unused] : item.items()) {
            static_cast<void>(unused);
            if (key != "criterion" && key != "path" && key != "observation") return corrupt();
        }
        PlannerNoChangesEvidenceItem evidenceItem {
            item["criterion"].get<std::string>(),
            item["path"].get<std::string>(),
            item["observation"].get<std::string>()};
        if (isBlank(evidenceItem.criterion) || isBlank(evidenceItem.path)
            || isBlank(evidenceItem.observation)
            || evidenceItem.criterion.size() > kMaximumNoChangesStringBytes
            || evidenceItem.path.size() > kMaximumNoChangesStringBytes
            || evidenceItem.observation.size() > kMaximumNoChangesStringBytes) {
            return corrupt();
        }
        result.evidence.push_back(std::move(evidenceItem));
    }
    if (parsed["recommendedNextStep"].is_string()) {
        auto recommendation = parsed["recommendedNextStep"].get<std::string>();
        if (recommendation.size() > kMaximumNoChangesStringBytes) return corrupt();
        if (!isBlank(recommendation)) result.recommendedNextStep = std::move(recommendation);
    }
    return common::Result<PlannerNoChangesNeeded>::ok(std::move(result));
}

// AG-18.3 hardening (blocker 2): typed identity round-trip for the
// verification-action-required-identity.v1.json artifact, mirroring
// renderPlannerNoChangesNeededJson/parsePlannerNoChangesNeededJson's own
// precedent - fails closed on any malformed/truncated input rather than
// returning a plausible-but-wrong default value.
std::string renderPlannerVerificationActionRequiredJson(
    const PlannerVerificationActionRequired& value)
{
    const nlohmann::json j {
        {"schemaVersion", value.schemaVersion},
        {"summary", value.summary},
        {"reason", value.reason},
        {"recommendedNextStep", value.recommendedNextStep.has_value()
            ? nlohmann::json(*value.recommendedNextStep)
            : nlohmann::json(nullptr)}};
    return j.dump();
}

common::Result<PlannerVerificationActionRequired> parsePlannerVerificationActionRequiredJson(
    std::string_view json)
{
    const auto corrupt = [] {
        return common::Result<PlannerVerificationActionRequired>::failure(
            {"planner_verification_action_required.corrupt_identity",
             "Malformed verification-action-required identity JSON"});
    };
    if (json.size() > kMaximumNoChangesSubmissionBytes) return corrupt();
    const auto parsed = nlohmann::json::parse(json, nullptr, false);
    if (parsed.is_discarded() || !parsed.is_object() || parsed.size() != 4) return corrupt();
    static const std::unordered_set<std::string> requiredFields {
        "schemaVersion", "summary", "reason", "recommendedNextStep"};
    for (const auto& [key, unused] : parsed.items()) {
        static_cast<void>(unused);
        if (!requiredFields.contains(key)) return corrupt();
    }
    if (!parsed.contains("schemaVersion") || !parsed["schemaVersion"].is_string()
        || parsed["schemaVersion"].get<std::string>()
            != std::string(kPlannerVerificationActionRequiredSchemaVersion)
        || !parsed.contains("summary") || !parsed["summary"].is_string()
        || !parsed.contains("reason") || !parsed["reason"].is_string()
        || !parsed.contains("recommendedNextStep")
        || !(parsed["recommendedNextStep"].is_null()
             || parsed["recommendedNextStep"].is_string())) {
        return corrupt();
    }
    PlannerVerificationActionRequired result;
    result.schemaVersion = std::string(kPlannerVerificationActionRequiredSchemaVersion);
    result.summary = parsed["summary"].get<std::string>();
    result.reason = parsed["reason"].get<std::string>();
    if (isBlank(result.summary) || result.summary.size() > kMaximumNoChangesStringBytes
        || isBlank(result.reason) || result.reason.size() > kMaximumNoChangesStringBytes) {
        return corrupt();
    }
    if (parsed["recommendedNextStep"].is_string()) {
        auto recommendation = parsed["recommendedNextStep"].get<std::string>();
        if (recommendation.size() > kMaximumNoChangesStringBytes) return corrupt();
        if (!isBlank(recommendation)) result.recommendedNextStep = std::move(recommendation);
    }
    return common::Result<PlannerVerificationActionRequired>::ok(std::move(result));
}

} // namespace ai::application
