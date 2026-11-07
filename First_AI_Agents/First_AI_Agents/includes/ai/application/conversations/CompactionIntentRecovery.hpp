#pragma once

#include "ai/application/conversations/CompactionIntentRecoveryReport.hpp"
#include "ai/application/conversations/IProjectMemoryCompactionIntentRepository.hpp"

#include <chrono>
#include <functional>

namespace ai::application {

// B3-II-B: what the owner of a database file does, once, at its startup, with the compaction intents a previous process left behind.
//
// An intent that is still `open` when the owner starts belongs to an operation that did not finish its recording protocol (a crash, a
// kill, or a storage refusal of the final write). The pass marks it `unresolved` - never deleting it, never guessing what happened
// to the request or the money - and leaves every already consistent row alone. It knows only the intent port: it has no provider, no
// client, no record repository, no ledger and no segment repository, so it cannot send, repeat, price or restore anything.
//
// One strict pair read (observeOperation) per row decides; a change counts only when the storage confirmed it, or when ONE more pair
// read shows exactly the expected transition. Anything else is `unconfirmed` and is not retried. A row's failure never stops the
// others. The permit is checked before anything else, and a refused call touches no port.
class CompactionIntentRecovery final {
public:
    using Clock = std::function<std::chrono::system_clock::time_point()>;

    // `clock` is read once per run (the instant written into every new mark, cut to whole milliseconds like the storage); empty means
    // the system clock.
    CompactionIntentRecovery(IProjectMemoryCompactionIntentRepository& intents, CompactionIntentRecoveryPermit permit, Clock clock = {});

    [[nodiscard]] CompactionIntentRecoveryReport run();

private:
    IProjectMemoryCompactionIntentRepository& intents_;
    CompactionIntentRecoveryPermit permit_;
    Clock clock_;
};

} // namespace ai::application
