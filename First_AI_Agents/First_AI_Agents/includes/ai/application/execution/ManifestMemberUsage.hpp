#pragma once

#include "ai/application/execution/ExecutionManifestTypes.hpp"
#include "ai/application/agents/ModelExecutionTypes.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace ai::application {

// AG-17.7B: member-level usage and cost of a Manifest Run, reconstructed - no schema
// change - from the Run's ModelExecution rows and its frozen Manifest + routing record.
//
// Why a reconstruction works: a ModelExecution row carries no member id, but its durable
// identity (source, sourceId, position, attempt) is allocated by one pure function of the
// frozen Manifest and routing record (allocateModelExecutionPositionsFromRecord - the
// writer's own code, reused here, never copied). Every attempt of a task, including a
// repair attempt, shares the task's position, so position -> task -> assignee names the
// member. The Planner row sits at the lead's position (role "planner") and the optional
// lead integration call at lastPosition + 1 (role "integration"); a worker's role is
// user-chosen text, which is why attribution is by POSITION and the role only
// disambiguates the two lead rows.
//
// The projection is a PARTITION of the Run's rows into: one bucket per routing member (that
// member's task executions), the lead's planning bucket, the lead's integration bucket
// and an `unattributed` bucket for every row that matches none of them. Nothing is ever
// dropped: an unattributed row is surfaced as diagnostic debt (it means the position
// allocation drifted from the persisted rows, or a row is foreign), and the projection
// verifies, field by field, that members + lead + unattributed == the Run total
// computed independently over every row.

enum class ManifestUsagePriceState {
    None,     // no execution at all
    Known,    // every execution was fully priced
    // A model price was unset for some usage category (or some execution). costMicros holds
    // the part that was priced, so it is a lower bound: either some executions were fully
    // priced, or the amount is positive even though every execution was incompletely priced.
    Partial,
    // Every execution was incompletely priced AND nothing could be priced (costMicros == 0):
    // there is no amount at all - the cost is NOT zero, it is unknown.
    Unknown,
};
[[nodiscard]] const char* toString(ManifestUsagePriceState state) noexcept;

// Sums of ModelExecution rows. Counters are saturating; `saturated` records that any of
// them hit the uint64 ceiling (the values are then lower bounds).
struct ManifestUsageTotals {
    // Every attempt of every status counts as one execution.
    std::uint64_t executions {0};
    std::uint64_t completed {0};
    std::uint64_t partial {0};
    std::uint64_t failed {0};
    std::uint64_t cancelled {0};
    // Pending + Running: an attempt that has not finished (normal in a live Run; in a
    // terminal Run the orphan sweep turns it into a failed row).
    std::uint64_t inFlight {0};
    // Failed rows the Backend failed because the attempt never finished: the application
    // restarted under it (kModelExecutionInterruptedErrorCode) or the Run ended under it
    // (kModelExecutionOrphanedErrorCode). A subset of `failed`, never in addition to it.
    std::uint64_t interrupted {0};
    std::uint64_t inputTokens {0};
    std::uint64_t outputTokens {0};
    // The best-effort sum of ModelExecution::costMicros over EVERY row, including rows
    // whose price was unknown. It is only a precise total when priceState() == Known.
    std::uint64_t costMicros {0};
    std::uint64_t unknownPriceExecutions {0};
    bool saturated {false};

    [[nodiscard]] ManifestUsagePriceState priceState() const noexcept;
    void addExecution(const ModelExecution& execution) noexcept;
    void add(const ManifestUsageTotals& other) noexcept;

    friend bool operator==(const ManifestUsageTotals&, const ManifestUsageTotals&) = default;
};

struct ManifestMemberUsage {
    std::string memberId;
    bool lead {false};
    // Only this member's TASK executions. The lead's planning/integration rows live in
    // ManifestUsageProjection::planning/integration, so the buckets stay disjoint.
    ManifestUsageTotals tasks;
};

struct ManifestUnattributedExecution {
    ModelExecutionId id;
    std::uint32_t position {0};
    std::uint32_t attempt {1};
    std::string role;
    std::string reason;
};

struct ManifestUsageProjection {
    // False when the Manifest carries no (or a defective) routing record: nothing can be
    // attributed, and nothing below is meaningful.
    bool available {false};
    // One entry per routing member, in record order.
    std::vector<ManifestMemberUsage> members;
    ManifestUsageTotals planning;
    ManifestUsageTotals integration;
    ManifestUsageTotals unattributed;
    std::vector<ManifestUnattributedExecution> unattributedExecutions;
    // The independent sum over every row of the Run.
    ManifestUsageTotals runTotal;
    // members + lead + unattributed == runTotal, field by field.
    bool partitionExact {false};
    // Stable "code: message" lines for every defect (see projectManifestMemberUsage).
    std::vector<std::string> diagnostics;

    // planning + integration.
    [[nodiscard]] ManifestUsageTotals lead() const noexcept;
    // Everything attributable to `memberId`: its task executions, plus the planning and
    // integration buckets when it is the lead (in a single-assignment Run the lone member
    // plans, executes and integrates). Empty totals for an unknown member.
    [[nodiscard]] ManifestUsageTotals totalFor(const std::string& memberId) const noexcept;
};

// Field-by-field check that members + planning + integration + unattributed equals
// runTotal. Returns one "usage.partition_mismatch: <field> ..." line per differing
// field; empty means the partition is exact. Exposed so a tampered projection can be
// proven to be caught.
[[nodiscard]] std::vector<std::string> usagePartitionMismatches(
    const ManifestUsageProjection& projection);

// Pure: no repository, no clock. `executions` are the Run's ModelExecution rows (every
// attempt, any status, any order). Diagnostics:
//   usage.position_plan_unavailable - the frozen Manifest and record do not yield a
//       position allocation (every worker row is then unattributed);
//   usage.unattributed              - at least one row matches no bucket;
//   usage.partition_mismatch        - the exactness check failed (a defect);
//   usage.counter_saturated         - a counter reached the uint64 ceiling.
[[nodiscard]] ManifestUsageProjection projectManifestMemberUsage(
    const ExecutionManifest& manifest,
    const std::vector<ModelExecution>& executions);

} // namespace ai::application
