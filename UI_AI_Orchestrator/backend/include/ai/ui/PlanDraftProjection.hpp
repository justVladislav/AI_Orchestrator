#pragma once

#include <ai/application/planning/NextRunPlan.hpp>
#include <ai/application/planning/RunDraftGroup.hpp>
#include <ai/application/planning/RunDraftTypes.hpp>

#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>

#include <optional>
#include <string>
#include <vector>

namespace ai::ui {

// PC-3D: how a Run Draft that came from a next-run plan is described to QML. Everything here is computed by the
// Backend side of the adapter from the stored draft and its group; QML only draws the result.

// "Edited since creation": the title, task, merged contracts or dependency keys no longer hash to the fingerprint
// sealed when the group was created. Roster, budget and workspace are deliberately not part of it. False for a manual
// draft (it has no sealed content).
[[nodiscard]] inline bool draftEditedSinceCreation(const ai::application::RunDraft& draft)
{
    if (!draft.provenance.has_value()) return false;
    return ai::application::nextRunPlanDraftContentFingerprint(
               draft.title, draft.task, draft.interfaceContracts, draft.provenance->dependsOn)
        != draft.provenance->createdContentFingerprint;
}

// The titles of the drafts `draft` should start after (advisory), one per dependency key and in the same order: the
// title of the sibling with that plan key, or "<key> (removed draft)" when that sibling no longer exists - a dependency
// on a deleted draft is stated as such, never shown as if it were a live one.
[[nodiscard]] inline QStringList draftDependencyTitles(
    const ai::application::RunDraft& draft, const std::vector<ai::application::RunDraft>& siblings)
{
    QStringList titles;
    if (!draft.provenance.has_value()) return titles;
    for (const std::string& key : draft.provenance->dependsOn) {
        QString title = QString::fromStdString(key) + QStringLiteral(" (removed draft)");
        for (const auto& sibling : siblings) {
            if (sibling.provenance.has_value() && sibling.provenance->groupId == draft.provenance->groupId
                    && sibling.provenance->planKey == key) {
                title = QString::fromStdString(sibling.title);
                break;
            }
        }
        titles.push_back(title);
    }
    return titles;
}

[[nodiscard]] inline QStringList toQStringList(const std::vector<std::string>& values)
{
    QStringList result;
    result.reserve(static_cast<qsizetype>(values.size()));
    for (const auto& value : values) result.push_back(QString::fromStdString(value));
    return result;
}

// The `planGroup` map of an opened draft; empty for a manual draft. {groupId, planTitle, origin, planKey, ordinal,
// dependsOn[], dependsOnTitles[], sourceRunIds[], edited}.
[[nodiscard]] inline QVariantMap planGroupMap(const ai::application::RunDraft& draft,
    const std::optional<ai::application::RunDraftGroup>& group, const std::vector<ai::application::RunDraft>& siblings)
{
    if (!draft.provenance.has_value()) return {};
    const auto& provenance = *draft.provenance;
    return QVariantMap {
        {QStringLiteral("groupId"), QString::fromStdString(provenance.groupId)},
        {QStringLiteral("planTitle"), group ? QString::fromStdString(group->planTitle) : QString {}},
        {QStringLiteral("origin"), group ? QString::fromLatin1(ai::application::toString(group->origin)) : QString {}},
        {QStringLiteral("planKey"), QString::fromStdString(provenance.planKey)},
        {QStringLiteral("ordinal"), static_cast<int>(provenance.ordinal)},
        {QStringLiteral("dependsOn"), toQStringList(provenance.dependsOn)},
        {QStringLiteral("dependsOnTitles"), draftDependencyTitles(draft, siblings)},
        {QStringLiteral("sourceRunIds"), group ? toQStringList(group->sourceRunIds) : QStringList {}},
        {QStringLiteral("edited"), draftEditedSinceCreation(draft)}};
}

} // namespace ai::ui
