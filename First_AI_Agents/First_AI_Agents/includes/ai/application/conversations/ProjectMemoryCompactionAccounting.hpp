#pragma once

#include "ai/application/conversations/ProjectMemoryCompactionOutcome.hpp"
#include "ai/application/conversations/ProjectMemoryCompactionPersistence.hpp"
#include "ai/application/conversations/ProjectMemoryCompactionTypes.hpp"
#include "ai/common/Error.hpp"
#include "ai/common/Result.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

// C2-B/S2b-2: the PURE policy of the compaction accounting. No I/O, no clock, no catalog: it decides, from what the storage reported
// about a write, whether the usage the provider already charged for may be recorded another way, and what each result means. The
// service (ProjectMemoryCompactionService) only wires the calls; every decision that can be wrong lives here and is tested alone.
namespace ai::application::compaction_accounting {

// What the accounting fallback row says when the primary failure has no usable code, and the fixed text of every fallback row.
inline constexpr const char* kFallbackDefaultErrorCode = "project_memory.compaction_persistence_failed";
inline constexpr const char* kFallbackErrorMessage =
    "The compacted summary could not be stored; the usage received from the provider is kept as a failed attempt";

// A stable backend code from the primary failure, or kFallbackDefaultErrorCode when that code is empty, too long or uses characters
// outside [A-Za-z0-9._-]. Never the primary MESSAGE: it may carry SQLite text.
[[nodiscard]] std::string safeFallbackErrorCode(std::string_view primaryCode);

// The Failed accounting row for an attempted record whose atomic commit failed: the SAME id, project, provider, model, ordered
// sources, source hash, creation time, input/output tokens, amount and price flag (nothing recalculated); status Failed; no result
// segment; a bounded code and a fixed text. The attempted record itself is not touched.
[[nodiscard]] ProjectMemoryCompactionRecord makeFailedAccountingRecord(
    const ProjectMemoryCompactionRecord& attempted, const common::Error& primary);

// What to do after the primary write of a Completed record failed.
struct FallbackDecision {
    bool attempt {false};                                                      // exactly ONE fallback insert
    CompactionAccounting accountingWithoutFallback {CompactionAccounting::Unknown};   // meaningful only when !attempt
    std::optional<CompactionDiagnostic> reason;                                // why no fallback was attempted
};

// attempt  <=>  the write is PROVEN not stored (NotCommitted) AND the connection is EXPLICITLY Healthy.
//   NotCommitted + any other / unobserved health -> no insert, NotSaved (this write is proven not stored; the connection is the reason
//                                                   nothing else is tried),
//   Unknown                                        -> no insert, Unknown,
//   Committed with a failed result                 -> contradictory: no insert, Unknown.
[[nodiscard]] FallbackDecision decideFallback(const CompactionPersistOutcome& primary);

// The meaning of the storage's verdict on a write that carries the received usage:
//   success + Committed                -> Saved,        failure + NotCommitted -> NotSaved,
//   failure + Unknown                  -> Unknown,
//   anything contradictory (a success that is not Committed, a failure that claims Committed) -> Unknown: never a guess.
[[nodiscard]] CompactionAccounting accountingOfWrite(const CompactionPersistOutcome& write) noexcept;

// The fallback insert failed because its id already exists AND the connection is explicitly Healthy: the only case in which the
// existing row is looked up (once, by id) instead of guessed about.
[[nodiscard]] bool fallbackCollidedWithExistingId(const CompactionPersistOutcome& fallback);

// The name of the first stored field in which `stored` differs from `expected`, or nullptr when they are identical. ALL fields take
// part: id, project, provider, model, ordered sources, hash, result segment, tokens, amount, the price flag (nullopt != false),
// status, error code and text, and the creation time at the precision the storage keeps (a millisecond). No current catalog price,
// no hash-only comparison, no unordered sources.
[[nodiscard]] const char* identityDifference(
    const ProjectMemoryCompactionRecord& expected, const ProjectMemoryCompactionRecord& stored);

struct CollisionResolution {
    CompactionAccounting accounting {CompactionAccounting::Unknown};
    // When the lookup itself failed: the ORIGINAL error of the lookup (its code and message, as the repository returned them),
    // as a separate typed cause on IdentityCheck. Nothing is recovered from a text.
    std::optional<CompactionDiagnostic> lookupCause;
    CompactionDiagnostic diagnostic;   // stage IdentityCheck: the verdict in words (an addition, never a replacement of the above)
};

// Reconciles a fallback that collided with an existing id using the ONE strict lookup of that id:
//   an identical row                          -> Saved (idempotent; nothing is written or overwritten),
//   a row that differs in any field / project -> NotSaved + project_memory.record_id_conflict,
//   the lookup failed / unsupported / the row is damaged, or no row exists although the id is "taken" -> Unknown.
[[nodiscard]] CollisionResolution resolveFallbackCollision(const ProjectMemoryCompactionRecord& expected,
    const common::Result<std::optional<ProjectMemoryCompactionRecord>>& lookup);

// The error of a failed accounting-fallback insert as its OWN cause (stage FallbackWrite): the original code and message. The
// repository returns that error only in its result, and a native detail is not a replacement for it.
[[nodiscard]] CompactionDiagnostic fallbackWriteCause(const common::Error& error);

// The secondary causes of the fallback WRITE, put into the phase of the fallback. A repository describes what it did in its own
// terms (Rollback, ProofRead, PrimaryWrite); the application, which knows that this write was the fallback, names the phase:
// Rollback -> FallbackRollback, ProofRead -> FallbackProofRead, PrimaryWrite -> FallbackWrite. Stages that already name the
// fallback or the identity check are kept. Order and content are untouched. The primary write's causes are not passed through this.
[[nodiscard]] std::vector<CompactionDiagnostic> inFallbackPhase(const std::vector<CompactionDiagnostic>& causes);

// Appends `from` to `into` in order, bounded: at most kMaxCompactionDiagnostics entries in total, each message at most
// kMaxCompactionDiagnosticMessageBytes without cutting a UTF-8 character. Anything dropped or shortened sets `incomplete`.
void appendDiagnostics(std::vector<CompactionDiagnostic>& into, bool& incomplete, const std::vector<CompactionDiagnostic>& from);
void appendDiagnostic(std::vector<CompactionDiagnostic>& into, bool& incomplete, CompactionDiagnostic diagnostic);

} // namespace ai::application::compaction_accounting
