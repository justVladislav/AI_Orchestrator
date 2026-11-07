#pragma once

#include <ai/application/conversations/CompactionIntentRecoveryReport.hpp>

#include <QVariantMap>

#include <optional>

// B3-III: the startup recovery report of the compaction intents as a QML map. The report belongs to the WHOLE DATABASE (every project)
// and to ONE moment - the owner's startup. It is not the current number of unfinished operations, and it is never filtered by a project
// or a period. Pure: no QObject, no I/O.
//
// Every key is always present; an absent value is an explicit absence, never 0/false:
//   scope                 always "database" (all projects, the startup of the owner)
//   applicable            true when the pass took part (a report exists). false: the pass did not take part (a guest, an in-memory
//                         database, a refused lock, unavailable storage) - that is NOT "complete" and NOT "zero rows"
//   status                "complete" | "partial" | "unavailable" | "none" (none: not applicable)
//   inspected, markedUnresolved, alreadyUnresolved, closedConfirmed, conflicts, unconfirmed   qulonglong; null when not applicable
//   hasFirstError, firstErrorCode, firstErrorMessage      the first cause, unchanged
//   firstNative           {present, code, extendedCode, message, text}; present=false when the storage gave none
//   diagnostics           [{stage, stageText, code, message}] in the order they happened
//   writeCauses           [{code, message, native{present,code,extendedCode,message,text}}] - an error of a write bound to ITS native detail,
//                         kept also when the transition was later confirmed
//   diagnosticsIncomplete a cause was lost or cut
//   channelErrorCode/channelErrorMessage   the separate startup error channel (first cause of a Partial/Unavailable report), "" if none
//   hasNotes              the report has diagnostics or write causes (shown as Details; a Complete report with notes stays Complete)
//   needsAttention        status partial or unavailable
//   hasActivity           inspected > 0
namespace ai::ui::compaction_recovery {

[[nodiscard]] QVariantMap reportMap(const std::optional<ai::application::CompactionIntentRecoveryReport>& report,
    const std::optional<ai::common::Error>& channelError);

} // namespace ai::ui::compaction_recovery
