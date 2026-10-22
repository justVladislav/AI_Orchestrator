#include <ai/ui/PromptWorkshopController.hpp>

#include <ai/application/RunDraftGroup.hpp>
#include <ai/ui/NextRunPlanVariant.hpp>
#include <ai/ui/RunContractVariant.hpp>

#include <QDateTime>

#include <algorithm>
#include <chrono>
#include <optional>
#include <utility>

namespace {

using ai::application::NextRunPlan;
using ai::application::RunDraftPlanCreation;
using ai::application::RunDraftPlanPreview;

QString text(const std::string& value) { return QString::fromStdString(value); }

QStringList strings(const std::vector<std::string>& values)
{
    QStringList result;
    result.reserve(static_cast<qsizetype>(values.size()));
    for (const auto& value : values) result.push_back(text(value));
    return result;
}

QString shortFingerprint(const std::string& fingerprint) { return text(fingerprint.substr(0, 12)); }

std::optional<ai::application::RunDraftId> templateOf(const QString& draftId)
{
    if (draftId.isEmpty()) return std::nullopt;
    return draftId.toStdString();
}

QVariantMap previewMap(const RunDraftPlanPreview& preview)
{
    const NextRunPlan& plan = preview.plan;
    // The same projection the follow-up composer's plan review uses (NextRunPlanVariant.hpp).
    const QVariantList drafts = ai::ui::nextRunPlanDraftsToVariant(plan);
    const QVariantList warnings = ai::ui::nextRunPlanWarningsToVariant(plan);
    QVariantMap overview = ai::ui::nextRunPlanStaffingOverviewToVariant(plan);
    overview.insert(QVariantMap {
        {QStringLiteral("ok"), true},
        {QStringLiteral("projectId"), text(preview.projectId)},
        {QStringLiteral("conversationId"), text(preview.conversationId)},
        {QStringLiteral("origin"), QString::fromLatin1(ai::application::toString(preview.origin))},
        {QStringLiteral("previewToken"), text(preview.previewToken)},
        {QStringLiteral("messageId"), preview.messageId ? text(*preview.messageId) : QString {}},
        {QStringLiteral("existingGroupId"), preview.existingGroupId ? text(*preview.existingGroupId) : QString {}},
        {QStringLiteral("planTitle"), text(plan.title)},
        {QStringLiteral("planFingerprint"), text(plan.fingerprint)},
        {QStringLiteral("shortFingerprint"), shortFingerprint(plan.fingerprint)},
        {QStringLiteral("sharedContractCount"), static_cast<int>(plan.sharedContracts.size())},
        {QStringLiteral("drafts"), drafts},
        {QStringLiteral("warnings"), warnings}});
    return overview;
}

QVariantMap creationMap(const RunDraftPlanCreation& creation)
{
    return ai::ui::runDraftGroupCreationToVariant(creation.group, creation.drafts, creation.alreadyExisted);
}

} // namespace

PromptWorkshopController::PromptWorkshopController(ai::application::RunDraftPlanService& service, QObject* parent)
    : QObject(parent)
    , service_(service)
{
}

QVariantMap PromptWorkshopController::fail(const ai::common::Error& error)
{
    reportError(error);
    return QVariantMap {
        {QStringLiteral("ok"), false},
        {QStringLiteral("errorCode"), text(error.code)},
        {QStringLiteral("errorMessage"), text(error.message)}};
}

void PromptWorkshopController::reportError(const ai::common::Error& error)
{
    lastError_ = text(error.message);
    emit lastErrorChanged();
    emit operationFailed(text(error.code), lastError_);
}

void PromptWorkshopController::clearError()
{
    if (lastError_.isEmpty()) return;
    lastError_.clear();
    emit lastErrorChanged();
}

QVariantMap PromptWorkshopController::previewFromMessage(const QString& conversationId, const QString& messageId)
{
    const auto preview = service_.previewFromMessage(conversationId.toStdString(), messageId.toStdString());
    if (!preview.success()) return fail(*preview.error());
    clearError();
    return previewMap(preview.value());
}

QVariantMap PromptWorkshopController::previewFromPaste(const QString& conversationId, const QString& pastedText)
{
    const auto preview = service_.previewFromPaste(conversationId.toStdString(), pastedText.toStdString());
    if (!preview.success()) return fail(*preview.error());
    clearError();
    return previewMap(preview.value());
}

QVariantMap PromptWorkshopController::createFromPreview(
    const QString& previewToken, const QString& rosterTemplateDraftId, const QStringList& sourceRunIds)
{
    std::vector<ai::application::RunId> runs;
    runs.reserve(static_cast<std::size_t>(sourceRunIds.size()));
    for (const QString& runId : sourceRunIds) runs.push_back(runId.toStdString());
    const auto created = service_.createFromPreview(previewToken.toStdString(), templateOf(rosterTemplateDraftId), runs);
    if (!created.success()) return fail(*created.error());
    clearError();
    if (!created.value().alreadyExisted) {
        emit draftsCreated(text(created.value().group.id), static_cast<int>(created.value().drafts.size()));
    }
    return creationMap(created.value());
}

QVariantMap PromptWorkshopController::rosterTemplates(const QString& conversationId)
{
    const auto templates = service_.listRosterTemplates(conversationId.toStdString());
    if (!templates.success()) return fail(*templates.error());
    clearError();
    QVariantList result;
    for (const auto& draft : templates.value()) {
        result.push_back(QVariantMap {
            {QStringLiteral("draftId"), text(draft.id)},
            {QStringLiteral("name"), text(draft.name)},
            {QStringLiteral("title"), text(draft.title)},
            {QStringLiteral("rosterSize"), static_cast<int>(draft.roster.size())},
            {QStringLiteral("workerCount"), static_cast<int>(std::count_if(draft.roster.begin(), draft.roster.end(),
                [](const ai::application::StageModelAssignment& agent) { return !agent.leadArchitect; }))},
            {QStringLiteral("maxParallelism"), static_cast<int>(draft.maxParallelism)}});
    }
    return QVariantMap {{QStringLiteral("ok"), true}, {QStringLiteral("templates"), result}};
}

QVariantMap PromptWorkshopController::sourceRunChoices(const QString& conversationId)
{
    const auto choices = service_.listSourceRunChoices(conversationId.toStdString());
    if (!choices.success()) return fail(*choices.error());
    clearError();
    QVariantList result;
    for (const auto& run : choices.value()) {
        const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(run.createdAt.time_since_epoch());
        result.push_back(QVariantMap {
            {QStringLiteral("runId"), text(run.id)},
            {QStringLiteral("title"), text(run.title)},
            {QStringLiteral("status"), QString::fromLatin1(ai::application::toString(run.status))},
            {QStringLiteral("createdAt"), QDateTime::fromMSecsSinceEpoch(milliseconds.count(), Qt::UTC)}});
    }
    return QVariantMap {{QStringLiteral("ok"), true}, {QStringLiteral("runs"), result}};
}
