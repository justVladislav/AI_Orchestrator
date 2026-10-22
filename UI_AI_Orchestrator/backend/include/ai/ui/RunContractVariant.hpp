#pragma once

#include <ai/application/RunInterfaceContract.hpp>

#include <QRegularExpression>
#include <QString>
#include <QStringList>
#include <QVariant>
#include <QVariantList>
#include <QVariantMap>

#include <string>
#include <vector>

namespace ai::ui {

// PC-2: the list-of-maps QML and the controllers exchange for a Run's interface contracts. Each map is
// {id, kind, title, text, appliesToPaths}; `kind` is "interface_contract" (the default when absent or empty) or
// "constraint" - any other text is refused with run_contracts.invalid_kind, never mapped to a default;
// `appliesToPaths` is a list of path-prefix strings or one free-text string split on commas, semicolons and line
// breaks (blank pieces of THAT form are dropped, and only spaces and tabs are trimmed around a piece). The
// Backend refuses a scope holding ',' or ';' or starting with a space, so the list form -> editor text -> free
// text form round trip is exact. QML only collects text: it never validates, trims or canonicalizes a contract -
// the Backend does (normalizeRunInterfaceContracts, run by RunManager and RunDraftService) and refuses a bad
// list with a typed run_contracts.* code.
[[nodiscard]] inline ai::common::Result<std::vector<ai::application::RunInterfaceContract>>
runContractsFromVariant(const QVariantList& contracts)
{
    using Contracts = std::vector<ai::application::RunInterfaceContract>;
    Contracts result;
    result.reserve(static_cast<std::size_t>(contracts.size()));
    for (const QVariant& entry : contracts) {
        const QVariantMap row = entry.toMap();
        ai::application::RunInterfaceContract contract;
        contract.id = row.value(QStringLiteral("id")).toString().toStdString();
        const QString kind = row.value(QStringLiteral("kind")).toString();
        if (!kind.isEmpty()
                && !ai::application::runContractKindFromString(kind.toStdString(), contract.kind)) {
            return ai::common::Result<Contracts>::failure({"run_contracts.invalid_kind",
                "Contract '" + contract.id + "' has an unknown kind (use interface_contract or constraint)"});
        }
        contract.title = row.value(QStringLiteral("title")).toString().toStdString();
        contract.text = row.value(QStringLiteral("text")).toString().toStdString();
        const QVariant scopes = row.value(QStringLiteral("appliesToPaths"));
        if (scopes.typeId() == QMetaType::QString) {
            const QStringList pieces = scopes.toString().split(
                QRegularExpression(QStringLiteral("[,;\\r\\n]+")), Qt::SkipEmptyParts);
            for (QString piece : pieces) {
                // Not QString::trimmed(): that also strips Unicode spaces, which the Backend keeps in a scope.
                while (piece.startsWith(QLatin1Char(' ')) || piece.startsWith(QLatin1Char('\t'))) piece.remove(0, 1);
                while (piece.endsWith(QLatin1Char(' ')) || piece.endsWith(QLatin1Char('\t'))) piece.chop(1);
                if (!piece.isEmpty()) contract.appliesToPaths.push_back(piece.toStdString());
            }
        } else {
            for (const QVariant& scope : scopes.toList()) {
                contract.appliesToPaths.push_back(scope.toString().toStdString());
            }
        }
        result.push_back(std::move(contract));
    }
    return ai::common::Result<Contracts>::ok(std::move(result));
}

[[nodiscard]] inline QVariantList runContractsToVariant(
    const std::vector<ai::application::RunInterfaceContract>& contracts)
{
    QVariantList result;
    result.reserve(static_cast<qsizetype>(contracts.size()));
    for (const auto& contract : contracts) {
        QVariantList scopes;
        for (const auto& scope : contract.appliesToPaths) {
            scopes.push_back(QString::fromStdString(scope));
        }
        result.push_back(QVariantMap {
            {QStringLiteral("id"), QString::fromStdString(contract.id)},
            {QStringLiteral("kind"), QString::fromLatin1(ai::application::toString(contract.kind))},
            {QStringLiteral("title"), QString::fromStdString(contract.title)},
            {QStringLiteral("text"), QString::fromStdString(contract.text)},
            {QStringLiteral("appliesToPaths"), scopes}});
    }
    return result;
}

} // namespace ai::ui
