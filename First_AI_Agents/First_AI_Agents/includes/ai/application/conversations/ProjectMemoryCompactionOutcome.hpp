#pragma once

#include "ai/application/conversations/ProjectMemoryCompactionIntentOutcome.hpp"
#include "ai/application/conversations/ProjectMemoryCompactionPersistence.hpp"
#include "ai/application/conversations/ProjectMemoryCompactionTypes.hpp"
#include "ai/common/Result.hpp"

#include <optional>
#include <vector>

namespace ai::application {

// C2-B/S2b-2: what ONE compaction call produced, beyond the Result that compact() has always returned. Neutral data only: no Qt, no
// SQL, no decision about how to show it. The service fills it; a later projection (S2b-3) reads it.

// Whether the usage/cost the call received from the provider reached the durable history. It is a statement about the RECORD that
// carries the received usage, never a statement about the primary operation (which keeps its own result).
//   NotApplicable  no provider response was received, so there is nothing to account for. NOT a promise that the call was free, and
//                  it never hides a persistence failure: that travels in `result`, `connection` and `diagnostics`.
//   Saved          a record carrying the received usage/cost/flag is stored (the normal record, or the Failed accounting fallback, or
//                  an identical row that already existed under the fallback's id).
//   NotSaved       PROVEN not stored: the storage said so and the proof backs it, or a different record owns the id.
//   Unknown        not proven either way. Never read as NotSaved (it may be stored) and never as Saved.
enum class CompactionAccounting { NotApplicable, Saved, NotSaved, Unknown };

struct ProjectMemoryCompactionOutcome {
    // Exactly what compact() returns. For a Completed commit that failed it is the PRIMARY failure, also when the accounting
    // fallback row was stored; a fallback never turns a failed compaction into a successful one.
    common::Result<ProjectMemoryCompactionRecord> result {
        common::Result<ProjectMemoryCompactionRecord>::failure({"project_memory.no_outcome", "No outcome was produced"})};

    // A successful provider response was received (a returned one, or the one a cancelled call reported as discarded). Stated
    // directly: it is never inferred from tokens, an amount or a price flag.
    bool responseObserved {false};
    CompactionAccounting accounting {CompactionAccounting::NotApplicable};

    // The record this call tried to store. If an accounting fallback was created, that fallback; if the Completed commit's fate is
    // Unknown, the Completed record as it was attempted (it is NOT rewritten into a Failed one that was never written). Absent when
    // the call failed before an attempt existed (validation, project/model lookup). Not a promise that it is in the history.
    std::optional<ProjectMemoryCompactionRecord> observedRecord;

    // The health of the shared connection as last observed by a storage call of this operation. nullopt = nothing observed; it is
    // never "healthy".
    std::optional<CompactionConnectionState> connection;

    // Native codes of the failed SQL step of the operation's own write (primary) and of the accounting fallback write (separate:
    // the fallback's never replaces the primary's).
    std::optional<CompactionNativeDetail> primaryNative;
    std::optional<CompactionNativeDetail> fallbackNative;

    // Secondary causes in the order they happened: entry recovery, the primary write's cleanup/proof, the fallback write, its
    // cleanup/proof, the identity check. At most kMaxCompactionDiagnostics entries of kMaxCompactionDiagnosticMessageBytes.
    std::vector<CompactionDiagnostic> diagnostics;
    // A cause was lost or cut (the list was full, a text was shortened, or a layer below could not record it).
    bool diagnosticsIncomplete {false};

    // B3-II-A: the intent of this call (admission before the request, the observation after the final writes). A separate axis: it
    // never changes `result`, `accounting` or `observedRecord`. A call that ended before a client existed (a stop, a refused
    // lookup, a failed client resolution) has admission NotApplicable.
    CompactionIntentTrace intent;
};

} // namespace ai::application
