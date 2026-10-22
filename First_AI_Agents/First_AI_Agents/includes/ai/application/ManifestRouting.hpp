#pragma once

#include "ai/application/ExecutionManifestTypes.hpp"
#include "ai/application/ExecutionProfileTypes.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ai::application {

// The longest memberId a routing record (and therefore a draft's
// assigneeMemberId) may carry.
inline constexpr std::size_t kManifestRoutingMaxMemberIdBytes = 80;

// AG-17.4: what the Planner is told about one roster member, supplied by the
// Backend from the Run's frozen assignment snapshot. `instructions` is the exact
// text whose SHA-256 the routing record froze, so a catalog that drifted from
// the record is detected (ManifestRouting::rosterCatalogProblem) instead of
// silently steering the Planner. `modelLabel` is display-only text (for example
// "OpenAI gpt-5.3-codex"): the provider/model identity itself lives in the
// record and is never chosen by the Planner.
struct ManifestRosterMemberInfo {
    std::string memberId;
    std::string modelLabel;
    std::string instructions;
};

// AG-17.1: the Backend-owned routing contract of a Manifest (manifest-routing.v1).
//
// Convergence direction: the multi-model roster becomes a front-end of the single
// Manifest workflow. A Manifest therefore needs a durable, immutable answer to
// "which roster member executes this provider-backed task, on which provider and
// model, under which role". This module owns that answer. It is pure: it reads no
// database, calls no provider and never mutates anything but its own argument.
//
// Authority split (the contract later slices build on):
//   - the provider names ONLY a memberId (ExecutionManifestTask::assigneeMemberId;
//     the strict draft schema gains that field in a later slice - in AG-17.1 the
//     Backend fills the record for a single-assignment Run, so behaviour is
//     unchanged);
//   - the Backend resolves provider/model/role/instructions/pricing from the
//     Run's frozen assignment snapshot and freezes a ManifestRoutingRecord on the
//     Manifest (member, provider, model, role, workspace capability, the SHA-256
//     of the instructions - never a second copy of the text);
//   - nothing re-derives it after freeze.
//
// Modes:
//   SingleAssignment - exactly one member (the Architect). A task without an
//     assignee means that member; an explicit assignee must equal it. Workspace
//     tools are implicitly granted to that member (the unchanged pre-AG-17
//     Manifest workflow), so no workspaceAccess check applies.
//   Roster - one lead Architect (never a task assignee) plus workers; every
//     provider-backed (Inspect/Propose) task carries an explicit assignee that
//     must be a worker with workspace access; a coordination (None) task has no
//     assignee. The data model allows several tasks per member; today's planning
//     rule "one task per worker" is a Planner/semantic rule (a later slice), not a
//     structural one, so a repair Manifest holding a subset of tasks stays valid.
class ManifestRouting final {
public:
    ManifestRouting() = delete;

    // Lower-case hex SHA-256 of the exact instruction text ("" hashes too).
    [[nodiscard]] static std::string instructionsHash(std::string_view instructions);

    // The role a routing member carries for `assignment`: the configured role, or
    // the member's stable id when none was configured. A saved execution profile
    // has no role column at all (every real Manifest-workflow Run assignment in
    // production has an empty role), and the roster runner has always treated an
    // empty role as its stage id - the record must be valid for every legal
    // assignment, so the same default applies here. Used identically by
    // fromAssignments and by every consumer that re-checks a route against the
    // record (TaskRouteResolver).
    [[nodiscard]] static std::string effectiveRole(const StageModelAssignment& assignment);

    // The member timeout in milliseconds: the configured positive timeout, or the
    // Backend's default turn timeout when none was configured (the same default
    // every runner has always applied to a non-positive assignment timeout).
    [[nodiscard]] static std::uint64_t effectiveTimeoutMs(const StageModelAssignment& assignment);

    // Backend-owned: builds the record from the Run's frozen assignment snapshot
    // (order preserved). One assignment -> SingleAssignment (that member is the
    // lead Architect regardless of its flag, since the Manifest workflow requires
    // exactly one Architect); several -> Roster (leadArchitect as configured).
    // The result's fingerprint is filled in.
    [[nodiscard]] static ManifestRoutingRecord fromAssignments(
        const std::vector<StageModelAssignment>& assignments);

    // Canonical SHA-256 over mode and the ordered members (length-prefixed fields,
    // so no field boundary is ambiguous).
    [[nodiscard]] static std::string computeFingerprint(const ManifestRoutingRecord& record);

    // Integrity of a record on its own: schema, size, mode/member-count/lead rules,
    // bounded non-empty ids, unique member ids and positions, well-formed
    // instruction hashes and a fingerprint that matches the members. nullopt =
    // consistent. The Backend is the only producer, so a problem is a Backend
    // defect and is never something a provider turn can repair.
    [[nodiscard]] static std::optional<std::string> recordProblem(
        const ManifestRoutingRecord& record);

    [[nodiscard]] static const ManifestRoutingMember* findMember(
        const ManifestRoutingRecord& record, std::string_view memberId);

    // AG-17.4: the member ids a Planner may name as a task assignee, in record
    // order: in a Roster record every worker (never the lead) with workspace
    // access - the only members the validator accepts for an Inspect/Propose task.
    // Empty for SingleAssignment (the sole member is implicit; the provider never
    // names it) and for a Roster without such a worker.
    [[nodiscard]] static std::vector<std::string> assignableMemberIds(
        const ManifestRoutingRecord& record);

    // AG-17.4: integrity of the catalog the Planner is shown, against the frozen
    // record. nullopt = consistent. SingleAssignment: the catalog must be empty.
    // Roster: exactly one entry per record member (no unknown, duplicate or
    // missing id), each entry's instruction text hashing to the record's
    // instructionsHash, a bounded single-line model label and a bounded total
    // size. A problem is a Backend defect (the catalog is Backend-built), never
    // something a provider turn can repair.
    [[nodiscard]] static std::optional<std::string> rosterCatalogProblem(
        const ManifestRoutingRecord& record,
        const std::vector<ManifestRosterMemberInfo>& catalog);

    // AG-17.4: the trusted roster section of the Planner's system prompt. Lists
    // exactly the assignable members (id, role, model label, instructions) in
    // record order and states the assignment contract. Empty for a record with no
    // assignable member (the Planner refuses such a roster before any session).
    // Precondition: rosterCatalogProblem(record, catalog) == nullopt.
    [[nodiscard]] static std::string renderRosterCatalog(
        const ManifestRoutingRecord& record,
        const std::vector<ManifestRosterMemberInfo>& catalog);

    // The member that executes `task`, or nullptr when it cannot be resolved (no
    // record, a coordination task, an unknown assignee, or a Roster task without
    // one). A SingleAssignment task without an assignee resolves to the sole member.
    [[nodiscard]] static const ManifestRoutingMember* assigneeOf(
        const ManifestRoutingRecord& record, const ExecutionManifestTask& task);

    // The Manifest validator's routing rules: the record's own integrity
    // (manifest.routing_invalid) and every task's assignee against it
    // (manifest.routing_assignee_unknown / _forbidden / _missing / _is_lead /
    // _capability_mismatch). Assignee findings are shaped so a Planner repair can
    // fix them; manifest.routing_invalid is not.
    [[nodiscard]] static std::vector<ExecutionManifestFinding> findings(
        const ExecutionManifest& manifest);
};

} // namespace ai::application
