// Unit tests for AG-17.7B's ManifestMemberUsage: the pure reconstruction of per-member usage
// and cost from a Run's ModelExecution rows plus its frozen Manifest and routing record.
// Row positions in these fixtures are computed BY HAND from the documented allocation (a
// member's first task keeps its frozen position, a repeated task takes
// maxMemberPosition + 1 + ordinal, the integration call takes lastPosition + 1; a single
// assignment uses lead position + 1 + task ordinal) so the tests do not lean on the code
// they check. The writer-side golden - the rows the real runner and executor produce - is
// asserted in ManifestWorkflowRunnerTests.
#include "ai/application/InMemoryRepositories.hpp"
#include "ai/application/execution/ManifestMemberUsage.hpp"
#include "ai/application/execution/ManifestRouting.hpp"
#include "ai/application/agents/ModelExecutionService.hpp"

#include <algorithm>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using namespace ai::application;

void require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

StageModelAssignment member(const std::string& stageType, const std::string& role, bool lead,
    bool workspace, std::uint32_t position)
{
    StageModelAssignment value;
    value.stageType = stageType;
    value.providerId = "provider-" + stageType;
    value.modelId = "model-" + stageType;
    value.role = role;
    value.leadArchitect = lead;
    value.workspaceAccess = workspace;
    value.position = position;
    value.timeout = std::chrono::milliseconds(60000);
    return value;
}

ExecutionManifestTask task(const std::string& id, WorkspaceCapability capability,
    std::optional<std::string> assignee)
{
    ExecutionManifestTask value;
    value.id = id;
    value.objective = "objective " + id;
    value.role = "task-role-" + id;
    value.requestedCapability = capability;
    value.assigneeMemberId = std::move(assignee);
    return value;
}

ExecutionManifest manifestFor(const std::vector<StageModelAssignment>& assignments,
    std::vector<ExecutionManifestTask> tasks)
{
    ExecutionManifest manifest;
    manifest.runId = "run-1";
    manifest.routing = ManifestRouting::fromAssignments(assignments);
    manifest.tasks = std::move(tasks);
    return manifest;
}

ModelExecution row(const std::string& id, std::uint32_t position, std::uint32_t attempt,
    const std::string& role, ModelExecutionStatus status = ModelExecutionStatus::Completed,
    std::uint64_t input = 10, std::uint64_t output = 5, std::uint64_t cost = 100,
    bool priceKnown = true, const std::string& errorCode = {})
{
    ModelExecution value;
    value.id = id;
    value.sourceId = "run-1";
    value.position = position;
    value.attempt = attempt;
    value.role = role;
    value.status = status;
    value.inputTokens = input;
    value.outputTokens = output;
    value.costMicros = cost;
    value.costPriceKnown = priceKnown;
    value.errorCode = errorCode;
    return value;
}

// lead@0, w1@1, w2@2, w3@3 (all workspace), doc@4 (no workspace access: never assigned).
std::vector<StageModelAssignment> rosterAssignments()
{
    return {member("lead", "Lead Architect", true, false, 0),
        member("w1", "QML implementer", false, true, 1),
        member("w2", "C++ implementer", false, true, 2),
        // A worker whose user-chosen role collides with the planner row's role tag.
        member("w3", "planner", false, true, 3),
        member("doc", "Writer", false, false, 4)};
}

// t1 -> w1, t2 -> w2, t3 -> w1 (repeat), t4 -> w3.
ExecutionManifest rosterManifest()
{
    return manifestFor(rosterAssignments(),
        {task("t1", WorkspaceCapability::Propose, "w1"),
         task("t2", WorkspaceCapability::Propose, "w2"),
         task("t3", WorkspaceCapability::Propose, "w1"),
         task("t4", WorkspaceCapability::Propose, "w3")});
}

// Roster allocation by hand: maxMemberPosition = 4. t1 = 1, t2 = 2, t3 (w1's second task)
// = 4 + 1 + 0 = 5, t4 = 3; lastPosition = 4 + 1 = 5, so the integration call is at 6.
std::vector<ModelExecution> rosterRows()
{
    return {
        row("planner", 0, 1, "planner", ModelExecutionStatus::Completed, 100, 50, 1000),
        row("t1-a1", 1, 1, "QML implementer", ModelExecutionStatus::Failed, 20, 0, 200),
        row("t1-a2", 1, 2, "QML implementer", ModelExecutionStatus::Completed, 30, 10, 300),
        row("t2-a1", 2, 1, "C++ implementer", ModelExecutionStatus::Completed, 40, 20, 400),
        row("t3-a1", 5, 1, "QML implementer", ModelExecutionStatus::Completed, 50, 25, 500),
        row("t4-a1", 3, 1, "planner", ModelExecutionStatus::Completed, 60, 30, 600),
        row("integration", 6, 1, "integration", ModelExecutionStatus::Completed, 70, 35, 700)};
}

const ManifestUsageTotals& tasksOf(const ManifestUsageProjection& projection, const char* memberId)
{
    const auto found = std::find_if(projection.members.begin(), projection.members.end(),
        [&](const ManifestMemberUsage& item) { return item.memberId == memberId; });
    require(found != projection.members.end(), "The projection lost a routing member");
    return found->tasks;
}

bool hasDiagnostic(const ManifestUsageProjection& projection, const char* prefix)
{
    return std::any_of(projection.diagnostics.begin(), projection.diagnostics.end(),
        [&](const std::string& line) { return line.rfind(prefix, 0) == 0; });
}

// ---- attribution ------------------------------------------------------------------

void testRosterRowsLandInTheirMembersByPosition()
{
    const auto projection = projectManifestMemberUsage(rosterManifest(), rosterRows());
    require(projection.available && projection.partitionExact && projection.diagnostics.empty()
            && projection.unattributed.executions == 0 && projection.unattributedExecutions.empty(),
        "A healthy roster Run must attribute every row with no diagnostics");
    require(projection.runTotal.executions == 7, "The Run total must count every row");

    require(projection.members.size() == 5 && projection.members[0].memberId == "lead"
            && projection.members[0].lead && projection.members[4].memberId == "doc",
        "Members must follow the routing record order");
    require(projection.planning.executions == 1 && projection.planning.inputTokens == 100
            && projection.integration.executions == 1 && projection.integration.inputTokens == 70,
        "The lead's planning and integration rows are told apart by position and role");
    const auto& w1 = tasksOf(projection, "w1");
    require(w1.executions == 3 && w1.completed == 2 && w1.failed == 1 && w1.inputTokens == 100
            && w1.outputTokens == 35 && w1.costMicros == 1000,
        "w1 owns both attempts of its first task and its second task (a repeated-task slot)");
    require(tasksOf(projection, "w2").executions == 1 && tasksOf(projection, "w2").inputTokens == 40,
        "w2 owns exactly its own task");
    const auto& w3 = tasksOf(projection, "w3");
    require(w3.executions == 1 && w3.inputTokens == 60,
        "A worker whose role is \"planner\" must be attributed by its position, never to the lead");
    require(tasksOf(projection, "lead").executions == 0 && tasksOf(projection, "doc").executions == 0,
        "A lead (which only plans) and a non-assignable member own no task executions");
}

void testSingleAssignmentUsesTheOrdinalAllocation()
{
    // Single assignment by hand: lead@5, base 6, tasks [t1 propose, t2 none, t3 propose]:
    // t1 = 6, t3 = 8 (the ordinal counts the coordination task), lastPosition = 8, so the
    // integration call is at 9.
    const auto manifest = manifestFor({member("architect", "Architect", true, true, 5)},
        {task("t1", WorkspaceCapability::Propose, std::nullopt),
         task("t2", WorkspaceCapability::None, std::nullopt),
         task("t3", WorkspaceCapability::Propose, std::nullopt)});
    const std::vector<ModelExecution> rows {
        row("planner", 5, 1, "planner"), row("t1", 6, 1, "task-role-t1"),
        row("t3", 8, 1, "task-role-t3"), row("integration", 9, 1, "integration")};
    const auto projection = projectManifestMemberUsage(manifest, rows);
    require(projection.available && projection.partitionExact && projection.diagnostics.empty(),
        "A single-assignment Run must attribute every row");
    require(projection.members.size() == 1 && projection.members[0].lead
            && projection.members[0].tasks.executions == 2 && projection.planning.executions == 1
            && projection.integration.executions == 1,
        "The lone member owns the task rows; planning and integration are the lead buckets");
    require(projection.totalFor("architect").executions == 4,
        "A single-assignment member plans, executes and integrates: its total is everything");
}

void testEveryAttemptAndStatusIsCounted()
{
    const auto manifest = manifestFor({member("architect", "Architect", true, true, 0)},
        {task("t1", WorkspaceCapability::Propose, std::nullopt)});
    // t1 is at position 1; each attempt is its own row.
    const std::vector<ModelExecution> rows {
        row("a1", 1, 1, "r", ModelExecutionStatus::Completed),
        row("a2", 1, 2, "r", ModelExecutionStatus::Partial),
        row("a3", 1, 3, "r", ModelExecutionStatus::Failed),
        row("a4", 1, 4, "r", ModelExecutionStatus::Failed, 1, 1, 1, true,
            kModelExecutionInterruptedErrorCode),
        row("a5", 1, 5, "r", ModelExecutionStatus::Failed, 1, 1, 1, true,
            kModelExecutionOrphanedErrorCode),
        row("a6", 1, 6, "r", ModelExecutionStatus::Cancelled),
        row("a7", 1, 7, "r", ModelExecutionStatus::Running),
        row("a8", 1, 8, "r", ModelExecutionStatus::Pending)};
    const auto projection = projectManifestMemberUsage(manifest, rows);
    const auto& totals = tasksOf(projection, "architect");
    require(projection.partitionExact && totals.executions == 8 && totals.completed == 1
            && totals.partial == 1 && totals.failed == 3 && totals.cancelled == 1
            && totals.inFlight == 2,
        "Every status of every attempt must be counted");
    require(totals.interrupted == 2,
        "Both kinds of interrupted rows (restart recovery and orphaned by the Run) are counted, "
        "as a subset of failed");
}

void testTheInterruptedCountFollowsTheRealRecoverySweeps()
{
    // The projection must count what ModelExecutionService really writes.
    InMemoryModelExecutionRepository repository;
    ModelExecutionService service {repository};
    const auto create = [&](std::uint32_t position, std::uint32_t attempt) {
        CreateModelExecutionRequest request;
        request.sourceId = "run-1";
        request.providerId = "p";
        request.modelId = "m";
        request.role = "r";
        request.position = position;
        request.attempt = attempt;
        const auto created = service.createExecution(request);
        require(created.success() && service.startExecution(created.value().id).success(),
            "Could not create a running execution");
    };
    create(1, 1);
    create(1, 2);
    require(service.recoverInterruptedExecutions().success(), "Restart recovery failed");
    create(1, 3);
    require(service.reconcileRunExecutions("run-1").success(), "Run reconciliation failed");

    const auto stored = repository.listExecutions(ModelExecutionSource::Run, "run-1");
    require(stored.success() && stored.value().size() == 3, "Expected three persisted rows");
    const auto manifest = manifestFor({member("architect", "Architect", true, true, 0)},
        {task("t1", WorkspaceCapability::Propose, std::nullopt)});
    const auto projection = projectManifestMemberUsage(manifest, stored.value());
    require(tasksOf(projection, "architect").failed == 3
            && tasksOf(projection, "architect").interrupted == 3 && projection.partitionExact,
        "Rows failed by restart recovery and by Run reconciliation must both count as interrupted");
}

// ---- price ------------------------------------------------------------------------

void testAnUnknownPriceIsNeverTreatedAsZero()
{
    const auto manifest = manifestFor({member("architect", "Architect", true, true, 0)},
        {task("t1", WorkspaceCapability::Propose, std::nullopt),
         task("t2", WorkspaceCapability::Propose, std::nullopt)});

    const auto none = projectManifestMemberUsage(manifest, {});
    require(tasksOf(none, "architect").priceState() == ManifestUsagePriceState::None
            && none.partitionExact && none.runTotal.executions == 0,
        "No execution is no price state, not a known zero");

    const auto known = projectManifestMemberUsage(manifest,
        {row("a", 1, 1, "r", ModelExecutionStatus::Completed, 1, 1, 100, true)});
    require(tasksOf(known, "architect").priceState() == ManifestUsagePriceState::Known,
        "Priced executions are a known cost");

    const auto partial = projectManifestMemberUsage(manifest,
        {row("a", 1, 1, "r", ModelExecutionStatus::Completed, 1, 1, 100, true),
         row("b", 2, 1, "r", ModelExecutionStatus::Completed, 1, 1, 0, false)});
    const auto& partialTotals = tasksOf(partial, "architect");
    require(partialTotals.priceState() == ManifestUsagePriceState::Partial
            && partialTotals.unknownPriceExecutions == 1 && partialTotals.costMicros == 100,
        "A mix of priced and unpriced executions is a partial cost: a lower bound");

    const auto unknown = projectManifestMemberUsage(manifest,
        {row("a", 1, 1, "r", ModelExecutionStatus::Completed, 1, 1, 0, false),
         row("b", 2, 1, "r", ModelExecutionStatus::Completed, 1, 1, 0, false)});
    const auto& unknownTotals = tasksOf(unknown, "architect");
    require(unknownTotals.priceState() == ManifestUsagePriceState::Unknown
            && unknownTotals.costMicros == 0 && unknownTotals.unknownPriceExecutions == 2,
        "All-unpriced executions must read as an UNKNOWN cost, never as a known zero");

    // CostBreakdown records the PRICED part of an incompletely priced execution (for example
    // unset cache prices next to a set input/output price). A positive amount with every
    // execution unpriced is therefore an honest lower bound, not "unknown".
    const auto allIncomplete = projectManifestMemberUsage(manifest,
        {row("a", 1, 1, "r", ModelExecutionStatus::Completed, 1, 1, 250, false)});
    const auto& incompleteTotals = tasksOf(allIncomplete, "architect");
    require(incompleteTotals.priceState() == ManifestUsagePriceState::Partial
            && incompleteTotals.costMicros == 250 && incompleteTotals.unknownPriceExecutions == 1
            && incompleteTotals.executions == 1,
        "An incompletely priced execution with a positive recorded cost is a partial lower bound");

    // A fully priced execution that cost nothing next to an unpriced one is still not "all
    // unknown", even though the recorded sum is zero.
    const auto zeroMix = projectManifestMemberUsage(manifest,
        {row("a", 1, 1, "r", ModelExecutionStatus::Completed, 1, 1, 0, true),
         row("b", 2, 1, "r", ModelExecutionStatus::Completed, 1, 1, 0, false)});
    require(tasksOf(zeroMix, "architect").priceState() == ManifestUsagePriceState::Partial
            && tasksOf(zeroMix, "architect").costMicros == 0,
        "A priced zero-cost execution next to an unpriced one is partial, not unknown");

    // The unknown rows of one bucket must not hide behind another bucket's known prices.
    const auto mixedBuckets = projectManifestMemberUsage(manifest,
        {row("planner", 0, 1, "planner", ModelExecutionStatus::Completed, 1, 1, 100, true),
         row("a", 1, 1, "r", ModelExecutionStatus::Completed, 1, 1, 0, false)});
    require(tasksOf(mixedBuckets, "architect").priceState() == ManifestUsagePriceState::Unknown
            && mixedBuckets.planning.priceState() == ManifestUsagePriceState::Known
            && mixedBuckets.totalFor("architect").priceState() == ManifestUsagePriceState::Partial,
        "Price state is computed per bucket, and a member total mixing them is partial");
    require(std::string(toString(ManifestUsagePriceState::Partial)) == "partial"
            && std::string(toString(ManifestUsagePriceState::Unknown)) == "unknown"
            && std::string(toString(ManifestUsagePriceState::Known)) == "known"
            && std::string(toString(ManifestUsagePriceState::None)) == "none",
        "Price state names are a UI vocabulary and must stay stable");
}

// ---- the partition and its debt ----------------------------------------------------

void testUnattributedRowsAreKeptAndReportedAsDebt()
{
    auto rows = rosterRows();
    rows.push_back(row("stray-slot", 99, 1, "stray", ModelExecutionStatus::Failed, 7, 3, 70, false));
    rows.push_back(row("wrong-role", 0, 2, "not-planner", ModelExecutionStatus::Completed, 8, 4, 80));
    rows.push_back(row("wrong-integration-role", 6, 2, "not-integration"));
    const auto projection = projectManifestMemberUsage(rosterManifest(), rows);

    require(projection.unattributed.executions == 3 && projection.unattributedExecutions.size() == 3,
        "Every row matching no bucket must be reported, none dropped");
    require(projection.unattributed.inputTokens == 7 + 8 + 10 && projection.unattributed.unknownPriceExecutions == 1
            && projection.unattributed.failed == 1,
        "The unattributed bucket carries the full usage of its rows");
    require(projection.runTotal.executions == 10 && projection.partitionExact,
        "The Run total includes the unattributed rows and the partition stays exact");
    require(projection.unattributedExecutions[0].id == "stray-slot"
            && projection.unattributedExecutions[0].position == 99
            && projection.unattributedExecutions[0].attempt == 1
            && projection.unattributedExecutions[0].role == "stray"
            && !projection.unattributedExecutions[0].reason.empty()
            && projection.unattributedExecutions[1].id == "wrong-role"
            && projection.unattributedExecutions[2].id == "wrong-integration-role",
        "Each unattributed row carries its identity and a reason, in input order");
    require(hasDiagnostic(projection, "usage.unattributed: 3 execution(s)"),
        "Unattributed rows are diagnostic debt and must be surfaced as such");
    // The healthy attribution is unchanged by the stray rows.
    require(tasksOf(projection, "w1").executions == 3 && projection.planning.executions == 1
            && projection.integration.executions == 1,
        "Stray rows must not disturb the attribution of the others");
}

void testAMissingPositionPlanLeavesWorkerRowsUnattributedButKeepsTheLeadRows()
{
    // A roster task without an assignee: no allocation can be derived (such a Manifest could
    // not freeze, but the read side must stay defensive).
    const auto manifest = manifestFor(rosterAssignments(),
        {task("t1", WorkspaceCapability::Propose, std::nullopt)});
    const auto projection = projectManifestMemberUsage(manifest, rosterRows());
    require(projection.available && hasDiagnostic(projection, "usage.position_plan_unavailable"),
        "An unavailable allocation must be reported");
    require(projection.planning.executions == 1 && projection.integration.executions == 0,
        "The planner row (lead position + role) survives; the integration slot is unknown");
    require(projection.unattributed.executions == 6 && projection.runTotal.executions == 7
            && projection.partitionExact,
        "Every row whose slot cannot be derived is unattributed, and none is lost");
}

void testTwoMembersSharingAPositionAreAmbiguousNotGuessed()
{
    // Two tasks with the same id assigned to different members: the allocation keeps one
    // slot for the id, and the projection must not pick a member for rows at that slot.
    const auto manifest = manifestFor(rosterAssignments(),
        {task("dup", WorkspaceCapability::Propose, "w1"),
         task("dup", WorkspaceCapability::Propose, "w2")});
    const auto projection = projectManifestMemberUsage(manifest,
        {row("x", 1, 1, "QML implementer")});
    require(projection.unattributed.executions == 1
            && projection.unattributedExecutions.front().reason.find("different members")
                != std::string::npos
            && tasksOf(projection, "w1").executions == 0 && tasksOf(projection, "w2").executions == 0,
        "A slot shared by tasks of different members must never be guessed");
}

void testThePartitionCheckCatchesATamperedProjection()
{
    const auto healthy = projectManifestMemberUsage(rosterManifest(), rosterRows());
    require(usagePartitionMismatches(healthy).empty(), "A healthy projection has no mismatch");

    auto doubleCounted = healthy;
    doubleCounted.members[1].tasks.addExecution(row("ghost", 1, 9, "r", ModelExecutionStatus::Failed,
        3, 2, 50, false, kModelExecutionOrphanedErrorCode));
    const auto mismatches = usagePartitionMismatches(doubleCounted);
    const auto mentions = [&](const char* field) {
        return std::any_of(mismatches.begin(), mismatches.end(), [&](const std::string& line) {
            return line.rfind("usage.partition_mismatch: ", 0) == 0
                && line.find(field) != std::string::npos;
        });
    };
    require(mentions("executions") && mentions("failed") && mentions("interrupted")
            && mentions("inputTokens") && mentions("outputTokens") && mentions("costMicros")
            && mentions("unknownPriceExecutions") && !mentions("completed"),
        "A row counted twice must be caught field by field");

    // Every status counter is checked on its own, not only through the row count.
    const std::vector<std::pair<const char*, ModelExecutionStatus>> statuses {
        {"completed", ModelExecutionStatus::Completed}, {"partial", ModelExecutionStatus::Partial},
        {"failed", ModelExecutionStatus::Failed}, {"cancelled", ModelExecutionStatus::Cancelled},
        {"inFlight", ModelExecutionStatus::Running}, {"inFlight", ModelExecutionStatus::Pending}};
    for (const auto& [field, status] : statuses) {
        auto tampered = healthy;
        // Hand-built so only the status counter and the row count differ.
        ManifestUsageTotals extra;
        extra.executions = 1;
        switch (status) {
        case ModelExecutionStatus::Completed: extra.completed = 1; break;
        case ModelExecutionStatus::Partial: extra.partial = 1; break;
        case ModelExecutionStatus::Failed: extra.failed = 1; break;
        case ModelExecutionStatus::Cancelled: extra.cancelled = 1; break;
        default: extra.inFlight = 1; break;
        }
        tampered.members[1].tasks.add(extra);
        const auto lines = usagePartitionMismatches(tampered);
        const bool named = std::any_of(lines.begin(), lines.end(), [&](const std::string& line) {
            return line.rfind(std::string("usage.partition_mismatch: ") + field + " ", 0) == 0;
        });
        require(named, "A tampered status counter must be named in the partition mismatch");
    }

    auto lost = healthy;
    lost.members[2].tasks = {};
    require(!usagePartitionMismatches(lost).empty(), "A row lost from every bucket must be caught");

    auto movedBetweenBuckets = healthy;
    std::swap(movedBetweenBuckets.members[1].tasks, movedBetweenBuckets.members[2].tasks);
    require(usagePartitionMismatches(movedBetweenBuckets).empty(),
        "Swapping two members is not a partition defect: the check is about totals");
}

// ---- boundaries -------------------------------------------------------------------

void testWithoutAUsableRoutingRecordNothingIsAttributed()
{
    ExecutionManifest legacy;
    legacy.runId = "run-1";
    legacy.tasks = {task("t1", WorkspaceCapability::Propose, std::nullopt)};
    require(!projectManifestMemberUsage(legacy, rosterRows()).available,
        "A Manifest frozen before routing records existed has no attribution");

    auto defective = rosterManifest();
    defective.routing->members.clear();
    const auto projection = projectManifestMemberUsage(defective, rosterRows());
    require(!projection.available && projection.members.empty() && projection.runTotal.executions == 0,
        "A defective record must not be trusted");

    // A record that is not empty but fails its own integrity check is distrusted too.
    auto sharedPosition = rosterManifest();
    sharedPosition.routing->members[2].position = sharedPosition.routing->members[1].position;
    require(!projectManifestMemberUsage(sharedPosition, rosterRows()).available,
        "A record with two members at one position must not be trusted");
    auto tamperedFingerprint = rosterManifest();
    tamperedFingerprint.routing->fingerprint = "not-the-fingerprint";
    require(!projectManifestMemberUsage(tamperedFingerprint, rosterRows()).available,
        "A record whose fingerprint does not match its members must not be trusted");
}

void testRowOrderDoesNotChangeTheProjection()
{
    auto rows = rosterRows();
    rows.push_back(row("stray", 77, 1, "stray"));
    const auto forward = projectManifestMemberUsage(rosterManifest(), rows);
    std::reverse(rows.begin(), rows.end());
    const auto backward = projectManifestMemberUsage(rosterManifest(), rows);
    require(forward.runTotal == backward.runTotal && forward.planning == backward.planning
            && forward.integration == backward.integration && forward.unattributed == backward.unattributed
            && forward.members.size() == backward.members.size(),
        "The projection must not depend on row order");
    for (std::size_t index = 0; index < forward.members.size(); ++index) {
        require(forward.members[index].tasks == backward.members[index].tasks,
            "A member's totals must not depend on row order");
    }
}

void testTotalForFoldsTheLeadBucketsIntoTheLeadOnly()
{
    const auto projection = projectManifestMemberUsage(rosterManifest(), rosterRows());
    require(projection.totalFor("lead").executions == 2 && projection.totalFor("lead").inputTokens == 170,
        "A roster lead's total is its planning and integration rows");
    require(projection.totalFor("w1") == tasksOf(projection, "w1"),
        "A worker's total is its task executions only");
    require(projection.totalFor("nobody") == ManifestUsageTotals {},
        "An unknown member has empty totals");
    require(projection.lead().executions == 2, "The lead buckets add up");
}

void testSaturatedCountersAreFlaggedNotWrapped()
{
    const auto manifest = manifestFor({member("architect", "Architect", true, true, 0)},
        {task("t1", WorkspaceCapability::Propose, std::nullopt)});
    const auto max = std::numeric_limits<std::uint64_t>::max();
    const auto projection = projectManifestMemberUsage(manifest,
        {row("a", 1, 1, "r", ModelExecutionStatus::Completed, max, 1, 1),
         row("b", 1, 2, "r", ModelExecutionStatus::Completed, 5, 1, 1)});
    require(projection.runTotal.inputTokens == max && projection.runTotal.saturated
            && tasksOf(projection, "architect").saturated
            && hasDiagnostic(projection, "usage.counter_saturated"),
        "An overflowing counter must saturate and be flagged, never wrap");
}

} // namespace

int runManifestMemberUsageTests()
{
    try {
        testRosterRowsLandInTheirMembersByPosition();
        testSingleAssignmentUsesTheOrdinalAllocation();
        testEveryAttemptAndStatusIsCounted();
        testTheInterruptedCountFollowsTheRealRecoverySweeps();
        testAnUnknownPriceIsNeverTreatedAsZero();
        testUnattributedRowsAreKeptAndReportedAsDebt();
        testAMissingPositionPlanLeavesWorkerRowsUnattributedButKeepsTheLeadRows();
        testTwoMembersSharingAPositionAreAmbiguousNotGuessed();
        testThePartitionCheckCatchesATamperedProjection();
        testWithoutAUsableRoutingRecordNothingIsAttributed();
        testRowOrderDoesNotChangeTheProjection();
        testTotalForFoldsTheLeadBucketsIntoTheLeadOnly();
        testSaturatedCountersAreFlaggedNotWrapped();
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "ManifestMemberUsage tests failed: " << exception.what() << '\n';
        return 1;
    }
}
