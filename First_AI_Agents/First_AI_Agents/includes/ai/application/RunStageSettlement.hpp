#pragma once

#include "ai/application/IRunRepository.hpp"

#include <chrono>
#include <optional>
#include <string>

namespace ai::application {

// AG-17.7D-1: the one rule that keeps a terminal Run from owning a stage that claims to be live.
//
// A stage row is driven only by the events a runner emits, so any path that ends a Run WITHOUT the
// runner finishing its stages (a crash, a Run that was only queued at shutdown or cancelled before
// it started, a runner that fails before its first StageStarted) would otherwise leave
// `pending` / `running` stages under a terminal Run for good. Every terminal transition therefore
// settles the Run's stages FIRST and the Run second: an interruption in between leaves a Run that
// is still unfinished, which startup recovery repeats idempotently.
//
//   Pending -> Skipped          always: the stage never started;
//   Running -> `runningBecomes` the caller's honest account of what happened to it (Failed for a
//                               lost or failed execution, Cancelled for a cancellation or a
//                               controlled shutdown); std::nullopt leaves a Running stage as it
//                               is (a Completed Run: a still-running stage there is a runner
//                               defect that must stay visible, never be papered over);
//   every already-terminal stage is left exactly as it is.
//
// Every settled stage gets the SAME `finishedAt` (the instant the Run itself ends) and `message`.
// Idempotent: a second call finds nothing live and writes nothing. `runningBecomes` must be a
// terminal stage status (Failed, Cancelled or Skipped).
[[nodiscard]] common::Result<void> settleLiveStages(
    IRunRepository& runs,
    const RunId& runId,
    std::optional<StageStatus> runningBecomes,
    std::chrono::system_clock::time_point finishedAt,
    const std::string& message);

} // namespace ai::application
