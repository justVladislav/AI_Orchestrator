#pragma once

#include "ai/application/conversations/ProjectMemoryCompactionPersistence.hpp"
#include "ai/common/Error.hpp"

#include <cstdint>
#include <functional>
#include <optional>
#include <utility>
#include <vector>

namespace ai::application {

// B3-II-B: the neutral vocabulary of the owner-startup recovery of the compaction intents. No SQL, no Qt, no provider.

// Stable machine code of an unresolved question that is not an error of the storage (a row that vanished, a state that cannot be
// concluded). Conflicts and read failures keep the codes of kIntent* in ProjectMemoryCompactionIntent.hpp and persistence.*.
inline constexpr const char* kIntentRecoveryUnconfirmedCode = "project_memory.intent_recovery_unconfirmed";
inline constexpr const char* kIntentRecoveryForbiddenMessage =
    "The compaction intent recovery may only run once, during the startup of the owner of the database file.";

// Complete     the pass ran to its end and every classification and every change it relied on is confirmed.
// Partial      the pass ran, but at least one row is a conflict or could not be concluded; the other rows were still handled.
// Unavailable  the pass could not rely on its source (a list could not be read, or the call was not permitted); nothing was changed.
// A default report is Unavailable: nothing was done and nothing is claimed.
enum class CompactionIntentRecoveryStatus { Complete, Partial, Unavailable };

[[nodiscard]] inline const char* toString(CompactionIntentRecoveryStatus status) noexcept
{
    switch (status) {
    case CompactionIntentRecoveryStatus::Complete: return "complete";
    case CompactionIntentRecoveryStatus::Partial: return "partial";
    case CompactionIntentRecoveryStatus::Unavailable: break;
    }
    return "unavailable";
}

// The native detail of a write whose error is kept as a cause, bound to THAT cause (code and message as the port gave them). It is
// kept whether or not a later read confirmed the transition the write was meant to make, so a Complete report still shows what the
// storage said about the write. It is not the first error and never stands in for another row's native detail.
struct CompactionIntentRecoveryWriteCause {
    common::Error cause;
    CompactionNativeDetail native;
};

// Counts of what was CONFIRMED, never of attempts. A row that is already consistent (a final record without an intent) changes
// nothing and is counted only in `inspected`.
struct CompactionIntentRecoveryReport {
    CompactionIntentRecoveryStatus status {CompactionIntentRecoveryStatus::Unavailable};
    std::uint64_t inspected {0};
    std::uint64_t markedUnresolved {0};      // an open intent confirmed to be marked by THIS pass (by the write, or by the one proof)
    std::uint64_t alreadyUnresolved {0};     // an intent that was unresolved before; its markedAt is untouched
    std::uint64_t closedConfirmed {0};       // an intent that coexisted with its matching final record and is confirmed closed
    std::uint64_t conflicts {0};             // an identity that contradicts the intent or the record; nothing was changed
    std::uint64_t unconfirmed {0};           // a row whose state or whose change could not be concluded; nothing more was tried
    // The first cause, as the storage or the policy gave it (code and message unchanged apart from the byte bound).
    std::optional<common::Error> firstError;
    std::optional<CompactionNativeDetail> firstNative;
    // Secondary causes in the order they happened, at most kMaxCompactionDiagnostics of kMaxCompactionDiagnosticMessageBytes.
    std::vector<CompactionDiagnostic> diagnostics;
    // The errors of mark/close writes together with their native details, in the order they happened, at most kMaxCompactionDiagnostics
    // (a dropped entry sets diagnosticsIncomplete). Present also when the write was later confirmed by the one pair read.
    std::vector<CompactionIntentRecoveryWriteCause> writeCauses;
    bool diagnosticsIncomplete {false};
};

// The runtime's statement that this call is the owner's startup recovery. A default value permits nothing; a value is only as good as
// the check it was built with, which is evaluated at the moment of the call. It is a runtime check, not a secret and not a token.
class CompactionIntentRecoveryPermit {
public:
    CompactionIntentRecoveryPermit() = default;
    explicit CompactionIntentRecoveryPermit(std::function<bool()> check) : check_(std::move(check)) {}

    [[nodiscard]] bool permits() const
    {
        return check_ != nullptr && check_();
    }

private:
    std::function<bool()> check_;
};

} // namespace ai::application
