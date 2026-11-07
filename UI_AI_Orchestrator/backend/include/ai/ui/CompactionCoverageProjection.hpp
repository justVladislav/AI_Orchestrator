#pragma once

#include <ai/application/conversations/CompactionCoverageTypes.hpp>

#include <QString>
#include <QVariantList>
#include <QVariantMap>

#include <optional>

// B3-III: how the Qt layer presents the compaction recording COVERAGE (Application: CompactionCoverageObservation) and the unfinished
// protocol rows (ProjectMemoryCompactionIntent). Pure functions: no QObject, no I/O, no SQL and no counting - the counts are the ones the
// Application computed from ONE read; nothing here derives them from the rows. The money is never involved: an intent has no amount.
namespace ai::ui::compaction_coverage {

// The coverage as a QML map. EVERY key is always present; an absent value is an explicit absence, never 0/false:
//   state             "available" | "unavailable" | "none"   (none: nothing was measured - no project, or the money read failed)
//   openOperations    qulonglong when state is available, otherwise null (an invalid QVariant)
//   unresolvedOperations   the same
//   hasCounts         true exactly when both counts are present
//   windowed          a bound of the period was requested; scope is "all_time" | "period" (the requested question, also for unavailable)
//   hasFrom/hasTo, fromText/toText   the requested bounds ("yyyy-MM-dd hh:mm" local time; "" when not given)
//   errorCode/errorMessage   the ORIGINAL error of the failed read ("" otherwise)
// nullopt gives state "none".
[[nodiscard]] QVariantMap coverageMap(const std::optional<ai::application::CompactionCoverageObservation>& coverage);

// One unfinished protocol: id, ids of the project/provider/model (data, not names), createdAt (text and ms), state "open" | "unresolved",
// sourceCount, markedAt (text, "" for open). No cost, no price state, no usage, no summary and no provider response: an intent has none.
[[nodiscard]] QVariantMap intentRowMap(const ai::application::ProjectMemoryCompactionIntent& intent);

// The intent SECTION of the project memory history: {projectId, state, coverage, rows, errorCode, errorMessage}. state mirrors the
// coverage (available | unavailable); rows are empty when unavailable. Both the coverage and the rows come from the one snapshot given.
[[nodiscard]] QVariantMap intentSectionMap(const QString& projectId, const ai::application::CompactionIntentReadSnapshot& snapshot);

} // namespace ai::ui::compaction_coverage
