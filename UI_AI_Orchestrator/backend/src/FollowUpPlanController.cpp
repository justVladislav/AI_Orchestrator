#include <ai/ui/FollowUpPlanController.hpp>

#include <ai/ui/NextRunPlanVariant.hpp>

#include <QDateTime>
#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>
#include <chrono>
#include <exception>
#include <functional>
#include <utility>

namespace {

namespace app = ai::application;

QString text(const std::string& value) { return QString::fromStdString(value); }
QString shortHash(const std::string& value) { return text(value.substr(0, 12)); }

QDateTime timeOf(std::chrono::system_clock::time_point value)
{
    const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(value.time_since_epoch());
    return QDateTime::fromMSecsSinceEpoch(milliseconds.count(), Qt::UTC);
}

QVariant optionalTime(const std::optional<std::chrono::system_clock::time_point>& value)
{
    return value.has_value() ? QVariant(timeOf(*value)) : QVariant(QDateTime {});
}

QVariantMap failureMap(const ai::common::Error& error)
{
    return QVariantMap {
        {QStringLiteral("ok"), false},
        {QStringLiteral("errorCode"), text(error.code)},
        {QStringLiteral("errorMessage"), text(error.message)}};
}

QVariantMap fileMap(const app::FollowUpFileView& file)
{
    return QVariantMap {
        {QStringLiteral("path"), text(file.relativePath)},
        {QStringLiteral("safety"), QString::fromLatin1(app::toString(file.safety))},
        {QStringLiteral("disposition"), QString::fromLatin1(app::toString(file.disposition))},
        {QStringLiteral("eligible"), file.safety == app::FollowUpFileSafety::Eligible},
        {QStringLiteral("includedByDefault"), file.disposition == app::FollowUpExcerptDisposition::Included},
        {QStringLiteral("sha256Short"), shortHash(file.contentSha256)},
        {QStringLiteral("originalBytes"), static_cast<qulonglong>(file.originalBytes)},
        {QStringLiteral("shownBytes"), static_cast<qulonglong>(file.shownBytes)},
        {QStringLiteral("estimatedTokens"), static_cast<qulonglong>(file.estimatedTokens)},
        {QStringLiteral("truncated"), file.truncated}};
}

QVariantList fileList(const std::vector<app::FollowUpFileView>& files)
{
    QVariantList rows;
    rows.reserve(static_cast<qsizetype>(files.size()));
    for (const auto& file : files) rows.push_back(fileMap(file));
    return rows;
}

QVariantMap kindMap(const app::FollowUpKindEligibility& kind)
{
    return QVariantMap {
        {QStringLiteral("offered"), kind.offered},
        {QStringLiteral("allowed"), kind.allowed},
        {QStringLiteral("reasonCode"), text(kind.reasonCode)},
        {QStringLiteral("reasonMessage"), text(kind.reasonMessage)}};
}

// The status of a Run that shows nothing: always the same complete shape.
QVariantMap emptyStatus(const QString& runId, const QString& availability)
{
    return QVariantMap {
        {QStringLiteral("availability"), availability},
        {QStringLiteral("runId"), runId},
        {QStringLiteral("projectId"), QString {}},
        {QStringLiteral("nextRun"), kindMap({})},
        {QStringLiteral("recovery"), kindMap({})},
        {QStringLiteral("requestPresent"), false},
        {QStringLiteral("request"), QVariantMap {}},
        {QStringLiteral("errorCode"), QString {}},
        {QStringLiteral("errorMessage"), QString {}}};
}

bool inFlight(const QVariantMap& status)
{
    if (!status.value(QStringLiteral("requestPresent")).toBool()) return false;
    const QString state = status.value(QStringLiteral("request")).toMap().value(QStringLiteral("state")).toString();
    return state == QLatin1String("authorized") || state == QLatin1String("composing");
}

} // namespace

FollowUpPlanController::FollowUpPlanController(
    ai::application::FollowUpPlanService& service, const ai::application::FollowUpPlanReadModel& readModel, QObject* parent)
    : QObject(parent)
    , service_(service)
    , readModel_(readModel)
    , status_(emptyStatus({}, QStringLiteral("not_applicable")))
{
    connect(&watcher_, &QFutureWatcher<AsyncOutcome>::finished, this, &FollowUpPlanController::finishAsync);
    poll_.setInterval(kPollMs);
    connect(&poll_, &QTimer::timeout, this, &FollowUpPlanController::refresh);
    selectionRead_.setSingleShot(true);
    selectionRead_.setInterval(kSelectionReadDelayMs);
    connect(&selectionRead_, &QTimer::timeout, this, &FollowUpPlanController::refresh);
}

FollowUpPlanController::~FollowUpPlanController()
{
    poll_.stop();
    selectionRead_.stop();
    // None of the reads of a running call can be interrupted; the services outlive this controller.
    watcher_.waitForFinished();
}

// ---- projections -----------------------------------------------------------------------------------------------------------------

QVariantMap FollowUpPlanController::requestMap(const app::FollowUpPlanRequest& request)
{
    const auto& quote = request.quote;
    const auto& pricing = quote.worstCase.pricingIdentity;
    QVariantList files;
    for (const auto& file : request.files) {
        files.push_back(QVariantMap {
            {QStringLiteral("path"), text(file.relativePath)},
            {QStringLiteral("sha256Short"), shortHash(file.contentSha256)},
            {QStringLiteral("originalBytes"), static_cast<qulonglong>(file.originalBytes)},
            {QStringLiteral("shownBytes"), static_cast<qulonglong>(file.shownBytes)},
            {QStringLiteral("estimatedTokens"), static_cast<qulonglong>(file.estimatedTokens)},
            {QStringLiteral("truncated"), file.truncated}});
    }
    QVariantMap failure;
    if (request.failure.has_value()) {
        failure = QVariantMap {
            {QStringLiteral("code"), text(request.failure->code)},
            {QStringLiteral("message"), text(request.failure->message)},
            {QStringLiteral("hasStoredReply"), request.failure->rawReply.has_value()}};
    }
    QVariantMap accounting;
    if (request.accounting.has_value()) {
        accounting = QVariantMap {
            {QStringLiteral("inputTokens"), static_cast<qulonglong>(request.accounting->inputTokens)},
            {QStringLiteral("outputTokens"), static_cast<qulonglong>(request.accounting->outputTokens)},
            {QStringLiteral("costMicros"), static_cast<qulonglong>(request.accounting->costMicros)},
            {QStringLiteral("priceKnown"), request.accounting->priceKnown}};
    }
    const QString planFingerprint = request.answer.has_value() ? text(request.answer->planFingerprint) : QString {};
    return QVariantMap {
        {QStringLiteral("id"), text(request.id)},
        {QStringLiteral("projectId"), text(request.projectId)},
        {QStringLiteral("sourceRunId"), text(request.sourceRunId)},
        {QStringLiteral("kind"), QString::fromLatin1(app::toString(request.kind))},
        {QStringLiteral("state"), QString::fromLatin1(app::toString(request.state))},
        {QStringLiteral("open"), app::isOpenFollowUpPlanRequestState(request.state)},
        {QStringLiteral("terminal"), app::isTerminalFollowUpPlanRequestState(request.state)},
        {QStringLiteral("mayBeCharged"), app::followUpRequestMayBeCharged(request)},
        {QStringLiteral("goal"), text(request.goal)},
        {QStringLiteral("maxDrafts"), static_cast<int>(request.maxDrafts)},
        {QStringLiteral("splitHint"), QString::fromLatin1(app::toString(request.splitHint))},
        {QStringLiteral("createdAt"), timeOf(request.createdAt)},
        {QStringLiteral("quoteExpiresAt"), timeOf(request.quoteExpiresAt)},
        {QStringLiteral("authorizedAt"), optionalTime(request.authorizedAt)},
        {QStringLiteral("claimedAt"), optionalTime(request.claimedAt)},
        {QStringLiteral("finishedAt"), optionalTime(request.finishedAt)},
        {QStringLiteral("quote"), QVariantMap {
            {QStringLiteral("providerId"), text(quote.providerIdentity.providerId)},
            {QStringLiteral("providerKind"), text(quote.providerIdentity.kind)},
            {QStringLiteral("baseUrl"), text(quote.providerIdentity.baseUrl)},
            {QStringLiteral("modelId"), text(pricing.modelId)},
            {QStringLiteral("apiModelId"), text(pricing.apiModelId)},
            {QStringLiteral("promptCache"), QString::fromLatin1(ai::models::toString(pricing.promptCache.mode).data())},
            {QStringLiteral("estimatedInputTokens"), static_cast<qulonglong>(quote.worstCase.estimatedInputTokens)},
            {QStringLiteral("maxOutputTokens"), static_cast<qulonglong>(quote.effectiveMaxOutputTokens())},
            {QStringLiteral("worstCaseCostMicros"), static_cast<qulonglong>(quote.worstCase.estimatedCostMicros)},
            {QStringLiteral("capMicros"), static_cast<qulonglong>(quote.capMicros)},
            {QStringLiteral("priceKnown"), quote.worstCase.priceKnown},
            {QStringLiteral("contextFingerprint"), shortHash(quote.contextFingerprint)}}},
        {QStringLiteral("files"), files},
        {QStringLiteral("failure"), failure},
        {QStringLiteral("accounting"), accounting},
        {QStringLiteral("draftGroupId"), request.draftGroupId.has_value() ? text(*request.draftGroupId) : QString {}},
        {QStringLiteral("planFingerprint"), planFingerprint},
        {QStringLiteral("shortPlanFingerprint"), planFingerprint.left(12)}};
}

QVariantMap FollowUpPlanController::statusMap(const app::FollowUpRunEligibility& view)
{
    using Availability = app::FollowUpRunEligibility::Availability;
    if (view.availability == Availability::NotApplicable) return emptyStatus(text(view.runId), QStringLiteral("not_applicable"));
    QVariantMap status = emptyStatus(text(view.runId), QStringLiteral("evaluated"));
    status.insert(QStringLiteral("projectId"), text(view.projectId));
    status.insert(QStringLiteral("nextRun"), kindMap(view.nextRun));
    status.insert(QStringLiteral("recovery"), kindMap(view.recovery));
    if (view.latestRequest.has_value()) {
        status.insert(QStringLiteral("requestPresent"), true);
        status.insert(QStringLiteral("request"), requestMap(*view.latestRequest));
    }
    return status;
}

// ---- status ----------------------------------------------------------------------------------------------------------------------

QVariantMap FollowUpPlanController::readStatus(const QString& runId)
{
    if (runId.isEmpty()) return emptyStatus({}, QStringLiteral("not_applicable"));
    const auto view = readModel_.eligibility(runId.toStdString());
    if (!view.success()) {
        QVariantMap status = emptyStatus(runId, QStringLiteral("unavailable"));
        status.insert(QStringLiteral("errorCode"), text(view.error()->code));
        status.insert(QStringLiteral("errorMessage"), text(view.error()->message));
        return status;
    }
    return statusMap(view.value());
}

void FollowUpPlanController::applyStatus(QVariantMap status)
{
    if (status != status_) {
        status_ = std::move(status);
        emit statusChanged();
    }
    updatePolling();
}

void FollowUpPlanController::updatePolling()
{
    if (inFlight(status_)) {
        if (!poll_.isActive()) poll_.start();
    } else {
        poll_.stop();
    }
}

void FollowUpPlanController::selectRun(const QString& runId)
{
    selectionRead_.stop();
    if (runId != selectedRunId_) {
        selectedRunId_ = runId;
        status_ = emptyStatus(runId, QStringLiteral("not_applicable"));
        poll_.stop();
        emit statusChanged();
    }
    refresh();
}

void FollowUpPlanController::selectRunSoon(const QString& runId)
{
    selectionRead_.stop();
    poll_.stop();
    if (runId != selectedRunId_) {
        selectedRunId_ = runId;
        status_ = emptyStatus(runId, QStringLiteral("not_applicable"));
        emit statusChanged();
    }
    if (!runId.isEmpty()) selectionRead_.start();
}

void FollowUpPlanController::refresh()
{
    applyStatus(readStatus(selectedRunId_));
}

// ---- pickers ---------------------------------------------------------------------------------------------------------------------

QVariantMap FollowUpPlanController::modelChoices()
{
    const auto choices = readModel_.modelChoices();
    if (!choices.success()) return fail(*choices.error());
    clearError();
    QVariantList models;
    for (const auto& choice : choices.value()) {
        models.push_back(QVariantMap {
            {QStringLiteral("providerId"), text(choice.providerId)},
            {QStringLiteral("providerName"), text(choice.providerName)},
            {QStringLiteral("providerKind"), text(choice.providerKind)},
            {QStringLiteral("modelId"), text(choice.modelId)},
            {QStringLiteral("displayName"), text(choice.displayName)},
            {QStringLiteral("usable"), choice.usable},
            {QStringLiteral("reasonCode"), text(choice.reasonCode)},
            {QStringLiteral("contextWindowTokens"), static_cast<qulonglong>(choice.contextWindowTokens)},
            {QStringLiteral("maxOutputTokens"), static_cast<qulonglong>(choice.maxOutputTokens)},
            {QStringLiteral("inputPriceMicrosPerMillionTokens"), static_cast<qulonglong>(choice.inputPriceMicrosPerMillionTokens)},
            {QStringLiteral("outputPriceMicrosPerMillionTokens"), static_cast<qulonglong>(choice.outputPriceMicrosPerMillionTokens)},
            {QStringLiteral("hasCredentialReference"), choice.hasCredentialReference}});
    }
    return QVariantMap {{QStringLiteral("ok"), true}, {QStringLiteral("models"), models}};
}

QVariantMap FollowUpPlanController::rosterTemplates(const QString& projectId)
{
    const auto templates = readModel_.rosterTemplates(projectId.toStdString());
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
                [](const app::StageModelAssignment& agent) { return !agent.leadArchitect; }))},
            {QStringLiteral("maxParallelism"), static_cast<int>(draft.maxParallelism)}});
    }
    return QVariantMap {{QStringLiteral("ok"), true}, {QStringLiteral("templates"), result}};
}

QVariantMap FollowUpPlanController::sourceRunChoices(const QString& projectId, const QString& requiredRunId)
{
    const auto choices = readModel_.sourceRunChoices(projectId.toStdString(), requiredRunId.toStdString());
    if (!choices.success()) return fail(*choices.error());
    clearError();
    QVariantList result;
    for (const auto& run : choices.value()) {
        result.push_back(QVariantMap {
            {QStringLiteral("runId"), text(run.id)},
            {QStringLiteral("title"), text(run.title)},
            {QStringLiteral("status"), QString::fromLatin1(app::toString(run.status))},
            {QStringLiteral("createdAt"), timeOf(run.createdAt)}});
    }
    return QVariantMap {{QStringLiteral("ok"), true}, {QStringLiteral("runs"), result}};
}

// ---- errors ----------------------------------------------------------------------------------------------------------------------

QVariantMap FollowUpPlanController::fail(const ai::common::Error& error)
{
    reportError(error);
    return failureMap(error);
}

void FollowUpPlanController::reportError(const ai::common::Error& error)
{
    lastError_ = text(error.message);
    emit lastErrorChanged();
    emit operationFailed(text(error.code), lastError_);
}

void FollowUpPlanController::clearError()
{
    if (lastError_.isEmpty()) return;
    lastError_.clear();
    emit lastErrorChanged();
}

quint64 FollowUpPlanController::refuse(const QString& code, const QString& message)
{
    reportError({code.toStdString(), message.toStdString()});
    return 0;
}

// ---- asynchronous commands ---------------------------------------------------------------------------------------------------------

void FollowUpPlanController::setBusy(bool value, const QString& operation)
{
    if (busy_ == value && busyOperation_ == operation) return;
    busy_ = value;
    busyOperation_ = operation;
    emit busyChanged();
}

quint64 FollowUpPlanController::startAsync(const QString& operation, std::function<void(AsyncOutcome&)> work)
{
    if (busy_) return refuse(QStringLiteral("follow_up.busy"), QStringLiteral("Another follow-up operation is still running."));
    clearError();
    const quint64 id = ++nextOperationId_;
    setBusy(true, operation);
    watcher_.setFuture(QtConcurrent::run([work = std::move(work), id, operation] {
        AsyncOutcome outcome;
        outcome.id = id;
        outcome.operation = operation;
        try {
            work(outcome);
        } catch (const std::exception& exception) {
            outcome.error = ai::common::Error {"follow_up.unexpected_failure", exception.what()};
        } catch (...) {
            outcome.error = ai::common::Error {"follow_up.unexpected_failure", "The follow-up operation failed unexpectedly."};
        }
        return outcome;
    }));
    return id;
}

void FollowUpPlanController::finishAsync()
{
    AsyncOutcome outcome = watcher_.result();
    setBusy(false, {});
    QVariantMap result;
    if (outcome.error.has_value()) {
        reportError(*outcome.error);
        result = failureMap(*outcome.error);
    } else {
        result.insert(QStringLiteral("ok"), true);
        if (outcome.quote.has_value()) {
            result.insert(QStringLiteral("request"), requestMap(outcome.quote->request));
            result.insert(QStringLiteral("replacedQuoteId"),
                outcome.quote->replacedQuoteId.has_value() ? text(*outcome.quote->replacedQuoteId) : QString {});
            result.insert(QStringLiteral("files"), fileList(outcome.quote->files));
        } else if (outcome.request.has_value()) {
            result.insert(QStringLiteral("request"), requestMap(*outcome.request));
        } else {
            result.insert(QStringLiteral("files"), fileList(outcome.files));
        }
    }
    // The status line follows the request the command just changed (or refused to change).
    refresh();
    emit operationFinished(outcome.id, outcome.operation, result);
}

quint64 FollowUpPlanController::describeFiles(const QString& projectId, const QString& sourceRunId, const QString& kind)
{
    const auto parsed = app::followUpPlanKindFromString(kind.toStdString());
    if (!parsed.has_value()) return refuse(QStringLiteral("follow_up.invalid_intent"), QStringLiteral("The kind of plan is not known."));
    auto* service = &service_;
    const std::string project = projectId.toStdString();
    const std::string run = sourceRunId.toStdString();
    const app::FollowUpPlanKind planKind = *parsed;
    return startAsync(QStringLiteral("describeFiles"), [service, project, run, planKind](AsyncOutcome& outcome) {
        auto described = service->describeFiles(project, run, planKind);
        if (!described.success()) {
            outcome.error = *described.error();
            return;
        }
        outcome.files = std::move(described.value());
    });
}

quint64 FollowUpPlanController::requestQuote(const QVariantMap& intentMap)
{
    app::FollowUpIntent intent;
    intent.projectId = intentMap.value(QStringLiteral("projectId")).toString().toStdString();
    intent.sourceRunId = intentMap.value(QStringLiteral("sourceRunId")).toString().toStdString();
    intent.goal = intentMap.value(QStringLiteral("goal")).toString().toStdString();
    intent.providerId = intentMap.value(QStringLiteral("providerId")).toString().toStdString();
    intent.modelId = intentMap.value(QStringLiteral("modelId")).toString().toStdString();

    const auto kind = app::followUpPlanKindFromString(intentMap.value(QStringLiteral("kind")).toString().toStdString());
    if (!kind.has_value()) return refuse(QStringLiteral("follow_up.invalid_intent"), QStringLiteral("The kind of plan is not known."));
    intent.kind = *kind;
    const auto split = app::followUpSplitHintFromString(intentMap.value(QStringLiteral("splitHint")).toString().toStdString());
    if (!split.has_value()) return refuse(QStringLiteral("follow_up.invalid_intent"), QStringLiteral("The split hint is not known."));
    intent.splitHint = *split;
    bool drafts = false;
    const qlonglong maxDrafts = intentMap.value(QStringLiteral("maxDrafts")).toLongLong(&drafts);
    if (!drafts || maxDrafts < 0 || maxDrafts > 1000) {
        return refuse(QStringLiteral("follow_up.invalid_intent"), QStringLiteral("The number of drafts is not a number."));
    }
    intent.maxDrafts = static_cast<std::uint32_t>(maxDrafts);
    // A cap that is present is the user's, whatever its value (the Backend refuses 0 and anything above the ceiling); none = the ceiling.
    if (intentMap.contains(QStringLiteral("capMicros")) && !intentMap.value(QStringLiteral("capMicros")).isNull()) {
        bool capOk = false;
        const qulonglong cap = intentMap.value(QStringLiteral("capMicros")).toULongLong(&capOk);
        if (!capOk) return refuse(QStringLiteral("follow_up.invalid_intent"), QStringLiteral("The cap is not a number."));
        intent.capMicros = static_cast<std::uint64_t>(cap);
    }
    if (intentMap.contains(QStringLiteral("selectedPaths")) && !intentMap.value(QStringLiteral("selectedPaths")).isNull()) {
        std::vector<std::string> paths;
        for (const QString& path : intentMap.value(QStringLiteral("selectedPaths")).toStringList()) paths.push_back(path.toStdString());
        intent.selectedPaths = std::move(paths);
    }

    auto* service = &service_;
    return startAsync(QStringLiteral("requestQuote"), [service, intent = std::move(intent)](AsyncOutcome& outcome) {
        auto quoted = service->quote(intent);
        if (!quoted.success()) {
            outcome.error = *quoted.error();
            return;
        }
        outcome.quote = std::move(quoted.value());
    });
}

quint64 FollowUpPlanController::authorize(const QString& requestId)
{
    if (requestId.isEmpty()) return refuse(QStringLiteral("follow_up.invalid_intent"), QStringLiteral("No request was named."));
    auto* service = &service_;
    const std::string id = requestId.toStdString();
    return startAsync(QStringLiteral("authorize"), [service, id](AsyncOutcome& outcome) {
        auto authorised = service->authorize(id);
        if (!authorised.success()) {
            outcome.error = *authorised.error();
            return;
        }
        outcome.request = std::move(authorised.value());
    });
}

// ---- synchronous commands ------------------------------------------------------------------------------------------------------------

QVariantMap FollowUpPlanController::cancel(const QString& requestId)
{
    const auto cancelled = service_.cancel(requestId.toStdString());
    if (!cancelled.success()) {
        refresh();
        return fail(*cancelled.error());
    }
    clearError();
    refresh();
    return QVariantMap {{QStringLiteral("ok"), true}, {QStringLiteral("request"), requestMap(cancelled.value())}};
}

QVariantMap FollowUpPlanController::discard(const QString& requestId)
{
    const auto discarded = service_.discard(requestId.toStdString());
    if (!discarded.success()) {
        refresh();
        return fail(*discarded.error());
    }
    clearError();
    refresh();
    return QVariantMap {{QStringLiteral("ok"), true}, {QStringLiteral("request"), requestMap(discarded.value())}};
}

QVariantMap FollowUpPlanController::preview(const QString& requestId)
{
    const auto previewed = service_.preview(requestId.toStdString());
    if (!previewed.success()) return fail(*previewed.error());
    const auto& value = previewed.value();
    if (!value.plan.has_value()) {
        return fail({"follow_up.not_plan_ready", "The request holds no plan to review."});
    }
    clearError();
    const app::NextRunPlan& plan = *value.plan;
    // The plan's own warnings first, then the request's advisories: one list, the way the Prompt Workshop shows its warnings.
    QVariantList warnings = ai::ui::nextRunPlanWarningsToVariant(plan);
    for (const auto& advisory : value.advisories) {
        warnings.push_back(QVariantMap {
            {QStringLiteral("code"), text(advisory.code)}, {QStringLiteral("message"), text(advisory.message)}});
    }
    QVariantMap overview = ai::ui::nextRunPlanStaffingOverviewToVariant(plan);
    overview.insert(QVariantMap {
        {QStringLiteral("ok"), true},
        {QStringLiteral("projectId"), text(value.request.projectId)},
        {QStringLiteral("sourceRunId"), text(value.request.sourceRunId)},
        {QStringLiteral("requestId"), text(value.request.id)},
        {QStringLiteral("origin"), QStringLiteral("follow_up_request")},
        {QStringLiteral("previewToken"), text(value.request.id)},
        {QStringLiteral("messageId"), QString {}},
        {QStringLiteral("existingGroupId"), value.request.draftGroupId.has_value() ? text(*value.request.draftGroupId) : QString {}},
        {QStringLiteral("planTitle"), text(plan.title)},
        {QStringLiteral("planFingerprint"), text(plan.fingerprint)},
        {QStringLiteral("shortFingerprint"), shortHash(plan.fingerprint)},
        {QStringLiteral("sharedContractCount"), static_cast<int>(plan.sharedContracts.size())},
        {QStringLiteral("drafts"), ai::ui::nextRunPlanDraftsToVariant(plan)},
        {QStringLiteral("warnings"), warnings},
        {QStringLiteral("request"), requestMap(value.request)}});
    return overview;
}

QVariantMap FollowUpPlanController::storedReply(const QString& requestId)
{
    const auto found = service_.find(requestId.toStdString());
    if (!found.success()) return fail(*found.error());
    if (!found.value().has_value()) return fail({"follow_up.not_found", "No such follow-up request"});
    const auto& request = *found.value();
    if (!request.failure.has_value() || !request.failure->rawReply.has_value()) {
        return fail({"follow_up.no_stored_reply", "This request has no stored reply."});
    }
    clearError();
    return QVariantMap {
        {QStringLiteral("ok"), true},
        {QStringLiteral("requestId"), text(request.id)},
        {QStringLiteral("code"), text(request.failure->code)},
        {QStringLiteral("message"), text(request.failure->message)},
        {QStringLiteral("reply"), text(*request.failure->rawReply)}};
}

QVariantMap FollowUpPlanController::confirm(const QString& requestId, const QString& planFingerprint,
    const QString& rosterTemplateDraftId, const QStringList& extraSourceRunIds, bool applySuggestedParallelism)
{
    app::FollowUpConfirmOptions options;
    options.applySuggestedParallelism = applySuggestedParallelism;
    if (!rosterTemplateDraftId.isEmpty()) options.rosterTemplate = rosterTemplateDraftId.toStdString();
    for (const QString& runId : extraSourceRunIds) options.extraSourceRunIds.push_back(runId.toStdString());
    const auto confirmed = service_.confirm(requestId.toStdString(), planFingerprint.toStdString(), options);
    if (!confirmed.success()) {
        refresh();
        return fail(*confirmed.error());
    }
    clearError();
    const auto& result = confirmed.value();
    refresh();
    if (result.created) emit draftsCreated(text(result.group.id), static_cast<int>(result.drafts.size()));
    return ai::ui::runDraftGroupCreationToVariant(result.group, result.drafts, !result.created);
}
