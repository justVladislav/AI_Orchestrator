#pragma once

#include <ai/application/planning/NextRunPlan.hpp>
#include <ai/application/planning/RunDraftGroup.hpp>
#include <ai/ui/RunContractVariant.hpp>

#include <QString>
#include <QStringList>
#include <QVariant>
#include <QVariantList>
#include <QVariantMap>

#include <algorithm>
#include <string>
#include <vector>

namespace ai::ui {

// The ONE projection of a parsed `next-run-plan.v1` into what the plan dialog (NextRunPlanDialog) renders, shared by the two Qt
// boundaries that feed it: the Prompt Workshop (a chat reply or a paste, PC-3C) and the follow-up composer (a stored paid answer,
// PC-5D). Both therefore show a plan in exactly the same shape; nothing here classifies, trims or repairs anything.
//
//   drafts:   [{key, title, task, dependsOn[], sharedContractIds[], sharedContractCount, contracts[],
//               staffing: {present, workerCount, parallelism, reason, summary, workers: [{role, focus}]}}]
//   warnings: [{code, message}]
[[nodiscard]] inline QStringList planStrings(const std::vector<std::string>& values)
{
    QStringList result;
    result.reserve(static_cast<qsizetype>(values.size()));
    for (const auto& value : values) result.push_back(QString::fromStdString(value));
    return result;
}

// The advisory staffing hint of one draft, worded by the Backend (QML only renders `summary`). `present` false = the plan carried
// none (or an invalid one, which the parser dropped with a warning).
[[nodiscard]] inline QVariantMap nextRunPlanStaffingToVariant(const ai::application::NextRunPlanDraft& draft)
{
    if (!draft.staffing.has_value()) {
        return QVariantMap {{QStringLiteral("present"), false}, {QStringLiteral("workerCount"), 0},
            {QStringLiteral("parallelism"), 0}, {QStringLiteral("reason"), QString {}}, {QStringLiteral("summary"), QString {}},
            {QStringLiteral("workers"), QVariantList {}}};
    }
    const auto& staffing = *draft.staffing;
    QVariantList workers;
    int implementation = 0;
    int documentation = 0;
    for (const auto& worker : staffing.workers) {
        workers.push_back(QVariantMap {{QStringLiteral("role"), QString::fromStdString(worker.role)},
            {QStringLiteral("focus"), QString::fromStdString(worker.focus)}});
        if (worker.role == "documentation") ++documentation; else ++implementation;
    }
    QStringList roles;
    if (implementation > 0) roles << QStringLiteral("%1 implementation").arg(implementation);
    if (documentation > 0) roles << QStringLiteral("%1 documentation").arg(documentation);
    const int count = static_cast<int>(staffing.workers.size());
    return QVariantMap {
        {QStringLiteral("present"), true},
        {QStringLiteral("workerCount"), count},
        {QStringLiteral("parallelism"), static_cast<int>(staffing.parallelism)},
        {QStringLiteral("reason"), QString::fromStdString(staffing.reason)},
        {QStringLiteral("summary"), QStringLiteral("%1 worker%2 (%3), parallelism %4")
            .arg(count).arg(count == 1 ? QString {} : QStringLiteral("s")).arg(roles.join(QStringLiteral(", "))).arg(staffing.parallelism)},
        {QStringLiteral("workers"), workers}};
}

// {hasStaffing, staffingMaxWorkers}: whether any draft carries a valid hint and the largest suggested worker count, for the
// plan-level choices of the dialog (the "use the suggested parallelism" tick and the template comparison).
[[nodiscard]] inline QVariantMap nextRunPlanStaffingOverviewToVariant(const ai::application::NextRunPlan& plan)
{
    int maxWorkers = 0;
    for (const auto& draft : plan.drafts) {
        if (draft.staffing.has_value()) maxWorkers = std::max(maxWorkers, static_cast<int>(draft.staffing->workers.size()));
    }
    return QVariantMap {{QStringLiteral("hasStaffing"), maxWorkers > 0}, {QStringLiteral("staffingMaxWorkers"), maxWorkers}};
}

[[nodiscard]] inline QVariantList nextRunPlanDraftsToVariant(const ai::application::NextRunPlan& plan)
{
    QVariantList drafts;
    for (const auto& draft : plan.drafts) {
        drafts.push_back(QVariantMap {
            {QStringLiteral("key"), QString::fromStdString(draft.key)},
            {QStringLiteral("title"), QString::fromStdString(draft.title)},
            {QStringLiteral("task"), QString::fromStdString(draft.task)},
            {QStringLiteral("dependsOn"), planStrings(draft.dependsOn)},
            {QStringLiteral("sharedContractIds"), planStrings(draft.sharedContractIds)},
            {QStringLiteral("sharedContractCount"), static_cast<int>(draft.sharedContractCount)},
            {QStringLiteral("contracts"), ai::ui::runContractsToVariant(draft.contracts)},
            {QStringLiteral("staffing"), nextRunPlanStaffingToVariant(draft)}});
    }
    return drafts;
}

[[nodiscard]] inline QVariantList nextRunPlanWarningsToVariant(const ai::application::NextRunPlan& plan)
{
    QVariantList warnings;
    for (const auto& warning : plan.warnings) {
        warnings.push_back(QVariantMap {
            {QStringLiteral("code"), QString::fromStdString(warning.code)},
            {QStringLiteral("message"), QString::fromStdString(warning.message)}});
    }
    return warnings;
}

// The creation map both write paths return after saving a Draft group:
//   {ok, alreadyExisted, groupId, projectId, origin, planTitle, planFingerprint, sourceRunIds[],
//    drafts: [{draftId, ordinal, planKey, title, dependsOn[], contractCount, rosterSize}]}
[[nodiscard]] inline QVariantMap runDraftGroupCreationToVariant(
    const ai::application::RunDraftGroup& group, const std::vector<ai::application::RunDraft>& drafts, bool alreadyExisted)
{
    QVariantList rows;
    for (const auto& draft : drafts) {
        const auto& provenance = draft.provenance;
        rows.push_back(QVariantMap {
            {QStringLiteral("draftId"), QString::fromStdString(draft.id)},
            {QStringLiteral("ordinal"), provenance ? static_cast<int>(provenance->ordinal) : -1},
            {QStringLiteral("planKey"), provenance ? QString::fromStdString(provenance->planKey) : QString {}},
            {QStringLiteral("title"), QString::fromStdString(draft.title)},
            {QStringLiteral("dependsOn"), provenance ? planStrings(provenance->dependsOn) : QStringList {}},
            {QStringLiteral("contractCount"), static_cast<int>(draft.interfaceContracts.size())},
            {QStringLiteral("rosterSize"), static_cast<int>(draft.roster.size())}});
    }
    return QVariantMap {
        {QStringLiteral("ok"), true},
        {QStringLiteral("alreadyExisted"), alreadyExisted},
        {QStringLiteral("groupId"), QString::fromStdString(group.id)},
        {QStringLiteral("projectId"), QString::fromStdString(group.projectId)},
        {QStringLiteral("origin"), QString::fromLatin1(ai::application::toString(group.origin))},
        {QStringLiteral("planTitle"), QString::fromStdString(group.planTitle)},
        {QStringLiteral("planFingerprint"), QString::fromStdString(group.planFingerprint)},
        {QStringLiteral("sourceRunIds"), planStrings(group.sourceRunIds)},
        {QStringLiteral("drafts"), rows}};
}

} // namespace ai::ui
