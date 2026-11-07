#pragma once

#include <ai/application/conversations/ProjectMemoryCompactionOutcome.hpp>

#include <QString>
#include <QVariantMap>

#include <cstdint>

// C2-B/S2b-3: the Qt PROJECTION of what ONE compaction call reported (ProjectMemoryCompactionOutcome). It only presents what the
// application already decided: no I/O, no SQL, no catalog, no price is recomputed or summed, and nothing is classified again (the
// accounting verdict, the connection health and the native codes are shown, never re-derived). Wording lives here (translated), so
// the QML block only displays the map and no contract is parsed out of a message.
//
// The map (every key is always present; an absent optional is an explicit "absent", never 0/false/healthy):
//   projectId               the project the call was ACCEPTED for (given by the caller; never read from the record, which can be absent)
//   resultSuccess           exactly Result::success of compact(). A returned Failed/Cancelled record is a success of the call, not of the
//                           compaction: operationState says what the operator should read
//   operationState          "completed" | "failed" | "cancelled"; a Result::failure is always "failed" (with the PRIMARY error)
//   headline                the operation in words
//   primaryCode/primaryMessage       the ORIGINAL error of the call ("" when the call returned a record)
//   recordStatus            status of the record compact() returned ("" when it returned an error)
//   observedRecordStatus    status of the record the call tried to store ("" when no attempt existed). Not a promise it is stored
//   terminalCode/terminalMessage     the reason carried by the Failed/Cancelled record (the returned one, else the attempted one);
//                           separate from primary: a cancelled compaction whose own record could not be stored shows BOTH
//   terminalOrigin          what confirms the record that carries that reason - ONLY the returned record or the accounting verdict Saved,
//                           never a diagnostic, a native detail or a text (those can be lost or cut):
//                             "stored"     a durable row (the record compact() returned, or the fallback the accounting confirms): its reason
//                                          is a recorded fact ("Recorded reason");
//                             "attempted"  any other observed Failed/Cancelled record: a CANDIDATE that may not be in the history (a failed
//                                          terminal insert, or the application's fallback row that NotSaved/Unknown does not confirm). Its own
//                                          words are never shown as a fact on the main screen (the fallback row's fixed text claims the usage
//                                          is kept);
//                             ""           no Failed/Cancelled record, no reason.
//                           terminalCode/terminalMessage are the raw data of that record; for "attempted" they are NOT for the main screen.
//   terminalText            the main-screen sentence ("" when none). "stored": the recorded reason (when it adds to the primary error).
//                           "attempted": only what the attempt did (failed / cancelled) and that its record is not confirmed saved - no raw text
//   terminalDetailText      the details line: the code of a stored record, or - for "attempted" - the ORIGINAL code and text labelled as
//                           the text of an attempted record that is not confirmed saved ("" when none)
//   responseObserved        a provider response was received
//   accounting              "saved" | "not_saved" | "unknown" | "not_applicable"
//   accountingCode/accountingMessage "project_memory.accounting_not_saved" / "..._unknown" + words; "" otherwise. They never replace
//                           primaryCode
//   accountingText          the fate of the received usage in words
//   observedCost            {available, priceState (none|known|partial|unknown), costText ("" unless known or partial), costMicros,
//                           priceIncomplete}; available only when a response was observed AND a record exists. Built with the stored
//                           record's own compactionPriceState; never from the current catalog
//   observedTokens          {available, inputTokens, outputTokens}
//   costLine                the amount, its completeness and whether it is saved, in ONE sentence ("" when not available)
//   connectionState         "healthy" | "quarantined" | "isolation_failed" | "unobserved"
//   connectionUsable        true | false | null (null = not observed; it is never "healthy")
//   connectionNeedsAttention  quarantined or isolation_failed
//   connectionText          the connection in words
//   primaryNative/fallbackNative  {present, code, extendedCode, message, text}; present=false when the storage reported none (not 0)
//   diagnostics             [{stage, stageText, code, message}] in the order they happened; every cause separate
//   diagnosticsIncomplete   a cause was lost or cut
//   diagnosticsIncompleteText the words for it ("" when complete)
//
// The map never carries a prompt, a summary of the answer, an HTTP request or a response body.
[[nodiscard]] QVariantMap projectMemoryOutcomeMap(
    const QString& projectId, const ai::application::ProjectMemoryCompactionOutcome& outcome);

// The amount of a stored compaction cost as the history shows it: "$" and four decimals (the same text as S1).
[[nodiscard]] QString compactionAmountText(std::uint64_t costMicros);

// The same words and maps the outcome uses, for the other compaction surfaces (the startup recovery report): the slug and the text of a
// diagnostic stage, and the native detail as {present, code, extendedCode, message, text} (present=false when none; never a 0).
[[nodiscard]] QString compactionDiagnosticStageSlug(ai::application::CompactionDiagnosticStage stage);
[[nodiscard]] QString compactionDiagnosticStageText(ai::application::CompactionDiagnosticStage stage);
[[nodiscard]] QVariantMap compactionNativeDetailMap(const std::optional<ai::application::CompactionNativeDetail>& detail);
