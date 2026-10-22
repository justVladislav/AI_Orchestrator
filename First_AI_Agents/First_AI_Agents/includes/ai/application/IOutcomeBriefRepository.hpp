#pragma once

#include "ai/application/OutcomeBrief.hpp"
#include "ai/common/Result.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace ai::application {

// PC-4B: the persistence port of the Outcome Brief. A Run's brief is stored as an APPEND-ONLY history of revisions plus
// one derived HEAD pointer per Run. Nothing is ever updated or deleted: a changed outcome is a NEW revision.
//
//   - The write gate is validateOutcomeBrief (PC-4A). A brief that is not exactly what deriveOutcomeBrief could have
//     produced is refused with `outcome_brief.invalid_brief` before storage is touched.
//   - Idempotency compares the brief's `sourceStateFingerprint` with the CURRENT head only, never with the whole history:
//     the head already says what is true now, so A -> B -> A is three revisions (the third is a real change back) and a
//     repeated A is no revision at all. There is deliberately no unique (run, fingerprint).
//   - Revisions of one Run have the ordinals 1, 2, 3, ... without gaps, and the head is always the one with the highest
//     ordinal. The head row is written in the same transaction as the revision.
//   - A writer states what head it expects (OutcomeBriefExpectedHead). The write happens only when the head is still that
//     one (compare-and-set), otherwise `outcome_brief.head_changed` and nothing is written.
//   - AUTHORITY. The Run row, not the brief, says which project a Run belongs to and what status it has now. A write checks both
//     (the Run exists, is the brief's project's, and still has the status the brief describes). EVERY read checks them again,
//     because a Run can leave its terminal status after the brief was written and a row can be made behind the repository's back:
//     the PROJECT of every revision must be the Run's project, and the terminal STATUS of the CURRENT revision (the head) must be the
//     Run's status now; older revisions describe what the Run was then and are not compared. A mismatch is `persistence.invalid_data`,
//     the code of a damaged row, so a consumer (PC-5) never receives a stale or foreign brief. The SQLite adapter joins `runs` on
//     every read; the in-memory adapter asks an injected lookup (see OutcomeBriefRunLookup).
using OutcomeBriefRevisionId = std::string;

// A hard ceiling for every list, the same idea as listRecentForProject's limit: the caller owns its own budget.
inline constexpr std::size_t kOutcomeBriefMaxListLimit = 1000;

struct OutcomeBriefRevision {
    OutcomeBriefRevisionId id;
    std::uint64_t ordinal {0};   // 1.. within the Run
    OutcomeBrief brief;
    std::chrono::system_clock::time_point createdAt;   // milliseconds
};

// What the caller believes the head of the Run is. `Any` skips the check and is ONLY for a single writer that does not care: two
// concurrent calculations (a hook and the startup pass, say) with `Any` let the older one overwrite the newer. A reconciler states
// `None` (no head yet) or `Revision(id)` with the id it got from `findHeadVersionForAppend` - also for a stale head.
struct OutcomeBriefExpectedHead {
    enum class Kind { Any, None, Revision };
    Kind kind {Kind::Any};
    OutcomeBriefRevisionId revisionId;   // Kind::Revision only

    [[nodiscard]] static OutcomeBriefExpectedHead any() { return {}; }
    [[nodiscard]] static OutcomeBriefExpectedHead none()
    {
        OutcomeBriefExpectedHead value;
        value.kind = Kind::None;
        return value;
    }
    [[nodiscard]] static OutcomeBriefExpectedHead revision(OutcomeBriefRevisionId id)
    {
        OutcomeBriefExpectedHead value;
        value.kind = Kind::Revision;
        value.revisionId = std::move(id);
        return value;
    }
};

// The authoritative facts about a Run that a brief must keep agreeing with.
struct OutcomeBriefRunOwner {
    ProjectId projectId;
    RunStatus status {RunStatus::Queued};
};
// What the in-memory adapter uses instead of the `runs` table. Empty optional: there is no such Run.
using OutcomeBriefRunLookup = std::function<std::optional<OutcomeBriefRunOwner>(const RunId&)>;

// What a WRITER may know about the head: which revision it is, never what it says (a stale brief must not reach a consumer).
struct OutcomeBriefHeadVersion {
    OutcomeBriefRevisionId revisionId;
    std::uint64_t ordinal {0};
};

struct OutcomeBriefAppendResult {
    OutcomeBriefRevision head;   // the head of the Run after the call (the new revision, or the unchanged one)
    bool appended {false};       // false: the brief had the head's source state, nothing was written
};

class IOutcomeBriefRepository {
public:
    virtual ~IOutcomeBriefRepository() = default;

    // Gate -> compare-and-set on the head -> same source state as the head? -> append revision + move head, all in ONE
    // transaction. Failure codes: `outcome_brief.invalid_brief` (the gate), `outcome_brief.invalid_request` (a malformed
    // expected head), `outcome_brief.head_changed`, and the Run authority (`outcome_brief.run_not_found`,
    // `outcome_brief.project_mismatch`, `outcome_brief.run_status_mismatch`; see AUTHORITY above). The head it compares with is
    // read WITHOUT the status check, so a head made stale by the Run moving on can always be replaced by a brief that is true now.
    virtual common::Result<OutcomeBriefAppendResult> append(OutcomeBrief brief, OutcomeBriefExpectedHead expected) = 0;

    // Every read below applies the READ authority (see above): `persistence.invalid_data` when a revision names another project than
    // its Run, or when the CURRENT revision describes a terminal status the Run no longer has. One such row fails a whole list.
    [[nodiscard]] virtual common::Result<std::optional<OutcomeBriefRevision>> findHead(const RunId& runId) const = 0;
    [[nodiscard]] virtual common::Result<std::optional<OutcomeBriefRevision>> findRevision(
        const OutcomeBriefRevisionId& id) const = 0;
    // The newest `limit` revisions of the Run, in ascending ordinal order. `limit` is 1..kOutcomeBriefMaxListLimit.
    [[nodiscard]] virtual common::Result<std::vector<OutcomeBriefRevision>> listRevisions(
        const RunId& runId, std::size_t limit) const = 0;
    // The newest `limit` heads of the project's Runs (newest first, ties by Run id descending). The authority is checked for the heads
    // of the RETURNED window only: a stale head outside it does not matter, one inside it fails the call.
    [[nodiscard]] virtual common::Result<std::vector<OutcomeBriefRevision>> listHeads(
        const ProjectId& projectId, std::size_t limit) const = 0;

    // The WRITER's read of the head: only its revision id and ordinal, to be handed back to `append` as
    // `OutcomeBriefExpectedHead::revision(id)`. Unlike `findHead` it does NOT require the head's terminal status to be the Run's
    // status now - a head that went stale because the Run moved on is exactly what a reconciler has to replace - but it still checks
    // the gate, that the Run exists and that the head names the Run's project, and it never returns the stale brief itself. Two
    // writers that saw the same head, stale or not, get one winner and `outcome_brief.head_changed` for the other. No head: empty -
    // for a Run that exists; a Run that does not exist is `persistence.invalid_data` whether or not a history is left behind.
    [[nodiscard]] virtual common::Result<std::optional<OutcomeBriefHeadVersion>> findHeadVersionForAppend(
        const RunId& runId) const = 0;
};

// ---- Shared by every adapter, so that the rules exist once ------------------------------------------------------------

// The request itself, before storage is read: the write gate and the shape of the expected head.
[[nodiscard]] common::Result<void> checkOutcomeBriefAppendRequest(
    const OutcomeBrief& brief, const OutcomeBriefExpectedHead& expected);

[[nodiscard]] common::Result<void> checkOutcomeBriefListLimit(std::size_t limit);

struct OutcomeBriefAppendPlan {
    bool appendRevision {false};
    std::uint64_t ordinal {0};   // the ordinal of the new revision, or of the unchanged head
};

// The compare-and-set and the idempotency rule against the CURRENT head (empty when the Run has none).
[[nodiscard]] common::Result<OutcomeBriefAppendPlan> planOutcomeBriefAppend(
    const OutcomeBrief& brief, const OutcomeBriefExpectedHead& expected, const std::optional<OutcomeBriefRevision>& head);

// WRITE authority: the Run exists (`outcome_brief.run_not_found`), is the brief's project's (`outcome_brief.project_mismatch`) and still has
// the status the brief describes (`outcome_brief.run_status_mismatch`).
[[nodiscard]] common::Result<void> checkOutcomeBriefWriteAuthority(
    const OutcomeBrief& brief, const std::optional<OutcomeBriefRunOwner>& owner);

// READ authority of a STORED revision: its project must be the Run's (always) and, when it is the CURRENT revision of the Run
// (`isHead`), its terminal status must be the Run's status now. Failure: `persistence.invalid_data`. `append` reads the head it
// compares with as `isHead = false`: a head that went stale because the Run moved on must not stop the NEW brief from replacing it.
[[nodiscard]] common::Result<void> checkOutcomeBriefReadAuthority(
    const OutcomeBrief& brief, const std::optional<OutcomeBriefRunOwner>& owner, bool isHead);

// The writer's read of a Run that has NO head yet: the Run itself must still exist, otherwise `persistence.invalid_data` (the code of
// every read whose Run is gone), so that "no head" never hides "no Run".
[[nodiscard]] common::Result<void> checkOutcomeBriefRunExists(bool runExists);

// A new revision with a fresh id and the current time (milliseconds, so a SQLite round trip is exact).
[[nodiscard]] OutcomeBriefRevision makeOutcomeBriefRevision(OutcomeBrief brief, std::uint64_t ordinal);

// The ordering of listHeads, applied by the in-memory adapter and expressed in SQL by the SQLite one.
[[nodiscard]] bool outcomeBriefHeadComesBefore(const OutcomeBriefRevision& left, const OutcomeBriefRevision& right);

} // namespace ai::application
