#include <ai/ui/ProjectMemoryOutcomeProjection.hpp>

#include <QCoreApplication>
#include <QVariant>
#include <QVariantList>

namespace {

using ai::application::CompactionAccounting;
using ai::application::CompactionConnectionState;
using ai::application::CompactionDiagnosticStage;
using ai::application::ExecutionPriceState;
using ai::application::ProjectMemoryCompactionStatus;

QString tr(const char* text)
{
    return QCoreApplication::translate("ProjectMemoryOutcomeProjection", text);
}

QString text(const std::string& value) { return QString::fromStdString(value); }

const char* statusSlug(ProjectMemoryCompactionStatus status) noexcept
{
    switch (status) {
    case ProjectMemoryCompactionStatus::Completed: return "completed";
    case ProjectMemoryCompactionStatus::Failed: return "failed";
    case ProjectMemoryCompactionStatus::Cancelled: return "cancelled";
    }
    return "failed";
}

const char* accountingSlug(CompactionAccounting value) noexcept
{
    switch (value) {
    case CompactionAccounting::Saved: return "saved";
    case CompactionAccounting::NotSaved: return "not_saved";
    case CompactionAccounting::Unknown: return "unknown";
    case CompactionAccounting::NotApplicable: break;
    }
    return "not_applicable";
}

const char* stageSlug(CompactionDiagnosticStage stage) noexcept
{
    switch (stage) {
    case CompactionDiagnosticStage::PrimaryWrite: return "primary_write";
    case CompactionDiagnosticStage::Rollback: return "rollback";
    case CompactionDiagnosticStage::ProofRead: return "proof_read";
    case CompactionDiagnosticStage::FallbackWrite: return "fallback_write";
    case CompactionDiagnosticStage::FallbackRollback: return "fallback_rollback";
    case CompactionDiagnosticStage::FallbackProofRead: return "fallback_proof_read";
    case CompactionDiagnosticStage::IdentityCheck: return "identity_check";
    case CompactionDiagnosticStage::IntentWrite: return "intent_write";
    case CompactionDiagnosticStage::IntentProofRead: return "intent_proof_read";
    case CompactionDiagnosticStage::IntentRecovery: return "intent_recovery";
    }
    return "primary_write";
}

QString stageText(CompactionDiagnosticStage stage)
{
    switch (stage) {
    case CompactionDiagnosticStage::PrimaryWrite: return tr("Write");
    case CompactionDiagnosticStage::Rollback: return tr("Rollback");
    case CompactionDiagnosticStage::ProofRead: return tr("Verification read");
    case CompactionDiagnosticStage::FallbackWrite: return tr("Fallback write");
    case CompactionDiagnosticStage::FallbackRollback: return tr("Fallback rollback");
    case CompactionDiagnosticStage::FallbackProofRead: return tr("Fallback verification read");
    case CompactionDiagnosticStage::IdentityCheck: return tr("Identity check");
    case CompactionDiagnosticStage::IntentWrite: return tr("Intent write");
    case CompactionDiagnosticStage::IntentProofRead: return tr("Intent verification read");
    case CompactionDiagnosticStage::IntentRecovery: return tr("Intent recovery");
    }
    return tr("Write");
}

QVariantMap nativeMap(const std::optional<ai::application::CompactionNativeDetail>& detail)
{
    if (!detail.has_value()) {
        return QVariantMap {{"present", false}, {"code", 0}, {"extendedCode", 0}, {"message", QString {}}, {"text", QString {}}};
    }
    return QVariantMap {
        {"present", true},
        {"code", detail->code},
        {"extendedCode", detail->extendedCode},
        {"message", text(detail->message)},
        {"text", tr("SQLite result %1 (extended %2): %3").arg(detail->code).arg(detail->extendedCode).arg(text(detail->message))}};
}

} // namespace

QString compactionAmountText(std::uint64_t costMicros)
{
    return QStringLiteral("$%1").arg(costMicros / 1'000'000.0, 0, 'f', 4);
}

QVariantMap projectMemoryOutcomeMap(const QString& projectId, const ai::application::ProjectMemoryCompactionOutcome& outcome)
{
    const bool success = outcome.result.success();
    const ai::application::ProjectMemoryCompactionRecord* returned = success ? &outcome.result.value() : nullptr;
    const ai::application::ProjectMemoryCompactionRecord* attempted = outcome.observedRecord ? &*outcome.observedRecord : nullptr;
    // The reason a Failed/Cancelled record carries: the returned record's, else the attempted one's. A Completed record carries none.
    const ai::application::ProjectMemoryCompactionRecord* terminal = returned != nullptr ? returned : attempted;
    const bool hasTerminalReason = terminal != nullptr && terminal->status != ProjectMemoryCompactionStatus::Completed;

    QVariantMap map;
    map["projectId"] = projectId;
    map["resultSuccess"] = success;

    QString operationState = QStringLiteral("failed");
    QString headline = tr("Compaction failed.");
    if (success) {
        switch (returned->status) {
        case ProjectMemoryCompactionStatus::Completed:
            operationState = QStringLiteral("completed");
            headline = tr("Memory compacted.");
            break;
        case ProjectMemoryCompactionStatus::Cancelled:
            operationState = QStringLiteral("cancelled");
            headline = tr("Compaction cancelled.");
            break;
        case ProjectMemoryCompactionStatus::Failed: break;
        }
    }
    map["operationState"] = operationState;
    map["headline"] = headline;
    map["primaryCode"] = success ? QString {} : text(outcome.result.error()->code);
    map["primaryMessage"] = success ? QString {} : text(outcome.result.error()->message);
    map["recordStatus"] = returned != nullptr ? QString::fromLatin1(statusSlug(returned->status)) : QString {};
    map["observedRecordStatus"] = attempted != nullptr ? QString::fromLatin1(statusSlug(attempted->status)) : QString {};
    map["terminalCode"] = hasTerminalReason ? text(terminal->errorCode) : QString {};
    map["terminalMessage"] = hasTerminalReason ? text(terminal->errorMessage) : QString {};

    // PROVENANCE of that reason, decided ONLY by what confirms a stored record: the returned record, or the accounting verdict Saved
    // (the fallback row that was written). Nothing else counts - not a diagnostic, not a native detail, not the text: those can be lost
    // or cut and must never open a stronger presentation. Every OTHER observed record is a CANDIDATE that may not be in the history, and
    // its own words (for the application's fallback row they claim the usage is kept) are never shown as a fact on the main screen:
    // the main screen says only what the attempt did (failed / cancelled) and that its record is not confirmed saved; the original
    // code and text stay whole in the details, labelled as the unconfirmed attempt's.
    //   stored | attempted | "" (no reason)
    QString origin;
    QString terminalText;
    QString terminalDetailText;
    if (hasTerminalReason) {
        const bool stored = returned != nullptr || outcome.accounting == CompactionAccounting::Saved;
        const QString message = text(terminal->errorMessage);
        const QString code = text(terminal->errorCode);
        if (stored) {
            const bool differsFromPrimary = success || code != text(outcome.result.error()->code) || message != text(outcome.result.error()->message);
            origin = QStringLiteral("stored");
            if (differsFromPrimary) terminalText = tr("Recorded reason: %1").arg(message);
            terminalDetailText = tr("Recorded reason code: %1").arg(code);
        } else {
            origin = QStringLiteral("attempted");
            terminalText = terminal->status == ProjectMemoryCompactionStatus::Cancelled
                ? tr("The compaction attempt was cancelled; its record is not confirmed saved.")
                : tr("The compaction attempt failed; its record is not confirmed saved.");
            terminalDetailText = tr("Original reason of the attempted record (not confirmed saved): %1 (%2)").arg(message, code);
        }
    }
    map["terminalOrigin"] = origin;
    map["terminalText"] = terminalText;
    map["terminalDetailText"] = terminalDetailText;

    // What happened to the usage the provider already charged for.
    map["responseObserved"] = outcome.responseObserved;
    map["accounting"] = QString::fromLatin1(accountingSlug(outcome.accounting));
    QString accountingCode;
    QString accountingMessage;
    QString accountingText;
    switch (outcome.accounting) {
    case CompactionAccounting::Saved:
        accountingText = success
            ? tr("The usage received from the provider is saved in the history.")
            : tr("The compaction was not completed. The usage received from the provider is saved in the history as a failed attempt.");
        break;
    case CompactionAccounting::NotSaved:
        accountingCode = QStringLiteral("project_memory.accounting_not_saved");
        accountingMessage = tr("The usage and cost of the provider response were not saved in the history.");
        accountingText = accountingMessage;
        break;
    case CompactionAccounting::Unknown:
        accountingCode = QStringLiteral("project_memory.accounting_unknown");
        accountingMessage = tr("It is not confirmed whether the usage and cost of the provider response were saved in the history.");
        accountingText = accountingMessage;
        break;
    case CompactionAccounting::NotApplicable:
        accountingText = tr("No provider response was received, so there was no usage to record.");
        break;
    }
    map["accountingCode"] = accountingCode;
    map["accountingMessage"] = accountingMessage;
    map["accountingText"] = accountingText;

    // The received usage and its estimate: only of the record the call itself tried to store, and only when a response exists.
    const bool costAvailable = outcome.responseObserved && attempted != nullptr;
    QVariantMap cost {{"available", false}, {"priceState", QStringLiteral("none")}, {"costText", QString {}},
        {"costMicros", QVariant::fromValue<qulonglong>(0)}, {"priceIncomplete", false}};
    QVariantMap tokens {{"available", false}, {"inputTokens", QVariant::fromValue<qulonglong>(0)},
        {"outputTokens", QVariant::fromValue<qulonglong>(0)}};
    QString costLine;
    if (costAvailable) {
        const auto priceState = ai::application::compactionPriceState(*attempted);
        const bool showsAmount = priceState == ExecutionPriceState::Known || priceState == ExecutionPriceState::Partial;
        const QString amount = compactionAmountText(attempted->costMicros);
        cost["available"] = true;
        cost["priceState"] = QString::fromLatin1(ai::application::toString(priceState));
        cost["costText"] = showsAmount ? amount : QString {};
        cost["costMicros"] = QVariant::fromValue<qulonglong>(attempted->costMicros);
        cost["priceIncomplete"] = priceState == ExecutionPriceState::Partial || priceState == ExecutionPriceState::Unknown;
        tokens["available"] = true;
        tokens["inputTokens"] = QVariant::fromValue<qulonglong>(attempted->inputTokens);
        tokens["outputTokens"] = QVariant::fromValue<qulonglong>(attempted->outputTokens);

        QString base;
        switch (priceState) {
        case ExecutionPriceState::Known:
            base = attempted->costMicros == 0
                ? tr("Estimated cost %1 (complete price; this is not proof that nothing was billed)").arg(amount)
                : tr("Estimated cost %1").arg(amount);
            break;
        case ExecutionPriceState::Partial:
            base = tr("Incomplete estimate: %1 recorded, total unknown").arg(amount);
            break;
        case ExecutionPriceState::Unknown:
            base = tr("Cost unknown (price incomplete)");
            break;
        case ExecutionPriceState::None:
            base = tr("No cost could be determined");
            break;
        }
        QString saveState;
        switch (outcome.accounting) {
        case CompactionAccounting::Saved: saveState = tr("saved"); break;
        case CompactionAccounting::NotSaved: saveState = tr("not saved"); break;
        case CompactionAccounting::Unknown: saveState = tr("saving not confirmed"); break;
        case CompactionAccounting::NotApplicable: break;
        }
        // The dash is spelled in UTF-8 bytes: a narrow literal with \u would depend on the compiler's execution character set.
        costLine = saveState.isEmpty() ? base : tr("%1 \xE2\x80\x94 %2").arg(base, saveState);
    }
    map["observedCost"] = cost;
    map["observedTokens"] = tokens;
    map["costLine"] = costLine;

    // The storage connection as last observed: an unobserved connection is neither healthy nor unusable.
    QString connectionState = QStringLiteral("unobserved");
    QString connectionText = tr("Storage connection: its state was not observed.");
    QVariant connectionUsable;   // null = not observed
    bool needsAttention = false;
    if (outcome.connection.has_value()) {
        switch (*outcome.connection) {
        case CompactionConnectionState::Healthy:
            connectionState = QStringLiteral("healthy");
            connectionText = tr("Storage connection: healthy.");
            connectionUsable = true;
            break;
        case CompactionConnectionState::Quarantined:
            connectionState = QStringLiteral("quarantined");
            connectionText = tr("Storage connection: isolated after a failed write. Database operations are refused until the interrupted transaction is closed.");
            connectionUsable = false;
            needsAttention = true;
            break;
        case CompactionConnectionState::IsolationFailed:
            connectionState = QStringLiteral("isolation_failed");
            connectionText = tr("Storage connection: unusable, and it could not be isolated.");
            connectionUsable = false;
            needsAttention = true;
            break;
        }
    }
    map["connectionState"] = connectionState;
    map["connectionUsable"] = connectionUsable;
    map["connectionNeedsAttention"] = needsAttention;
    map["connectionText"] = connectionText;

    map["primaryNative"] = nativeMap(outcome.primaryNative);
    map["fallbackNative"] = nativeMap(outcome.fallbackNative);

    QVariantList diagnostics;
    for (const auto& entry : outcome.diagnostics) {
        diagnostics.push_back(QVariantMap {
            {"stage", QString::fromLatin1(stageSlug(entry.stage))},
            {"stageText", stageText(entry.stage)},
            {"code", text(entry.error.code)},
            {"message", text(entry.error.message)}});
    }
    map["diagnostics"] = diagnostics;
    map["diagnosticsIncomplete"] = outcome.diagnosticsIncomplete;
    map["diagnosticsIncompleteText"] = outcome.diagnosticsIncomplete
        ? tr("The diagnostics are incomplete: at least one cause was lost or shortened.") : QString {};
    return map;
}
