#include "../../test_support/QtTestSupport.hpp"

// The Backend-side fixtures of the follow-up service tests (a scripted source reader and provider over the REAL in-memory repositories):
// the controller is driven over exactly the Backend the service tests prove.
#include "FollowUpServiceTestSupport.hpp"

#include <ai/application/FollowUpPlanReadModel.hpp>
#include <ai/application/InMemoryOutcomeBriefRepository.hpp>
#include <ai/application/OutcomeBrief.hpp>
#include <ai/backend/BackendRuntime.hpp>
#include <ai/infrastructure/SqliteRepositories.hpp>
#include <ai/ui/BackendBridge.hpp>
#include <ai/ui/FollowUpPlanController.hpp>

#include <QCoreApplication>
#include <QDateTime>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>

#include <chrono>
#include <filesystem>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

// PC-5D: the Qt boundary of the opt-in paid next-Run composer. The controller only copies what FollowUpPlanService and
// FollowUpPlanReadModel say into QVariantMaps and relays commands, so these tests pin (1) the shape and the values of every map, (2)
// that a quote, an authorisation, a cancel, a discard and a confirm are the Backend's own (the provider is called by the Backend's
// processor only for an authorised request, never by the controller), (3) the asynchronous contract (one at a time, results through
// operationFinished, the status line following the request), (4) the guest refusal, and (5) the wiring on a real BackendBridge.

namespace {

namespace fs = std::filesystem;
namespace app = ai::application;
namespace fst = followup_service_test;
using namespace std::chrono_literals;

void require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}
void require(bool condition, const std::string& message) { require(condition, message.c_str()); }

QString q(const std::string& value) { return QString::fromStdString(value); }

class StaticRuns final : public app::IRunSourceLookup {
public:
    [[nodiscard]] ai::common::Result<std::optional<app::ProjectId>> findRunProject(const app::RunId& runId) const override
    {
        const auto found = projects.find(runId);
        return ai::common::Result<std::optional<app::ProjectId>>::ok(
            found == projects.end() ? std::nullopt : std::optional<app::ProjectId>(found->second));
    }
    [[nodiscard]] ai::common::Result<std::vector<app::RunSourceChoice>> listProjectRuns(const app::ProjectId&) const override
    {
        return ai::common::Result<std::vector<app::RunSourceChoice>>::ok(choices);
    }
    std::map<app::RunId, app::ProjectId> projects;
    std::vector<app::RunSourceChoice> choices;
};

app::OutcomeBrief briefFor(const std::string& runId, app::RunStatus status)
{
    app::ManifestWorkLedgerEntry ledger;
    ledger.runId = runId;
    ledger.projectId = fst::kProject;
    ledger.terminalStatus = status;
    ledger.disposition = status == app::RunStatus::Completed ? app::ManifestWorkLedgerDisposition::NoChangesNeeded
                                                             : app::ManifestWorkLedgerDisposition::NotProduced;
    ledger.baselineFingerprint = std::string(64, 'a');
    app::OutcomeBriefFacts facts;
    facts.runId = runId;
    facts.projectId = fst::kProject;
    facts.workflowId = app::kExecutionManifestWorkflowId;
    facts.terminalStatus = status;
    facts.ledgerEntry = ledger;
    const auto derived = app::deriveOutcomeBrief(facts);
    require(derived.success(), "setup: the brief derives");
    return derived.value();
}

// The controller over the Backend of the service tests plus the read model's own repositories (briefs and a Run lookup).
struct Rig {
    fst::Harness h;
    std::map<app::RunId, app::OutcomeBriefRunOwner> owners;
    app::InMemoryOutcomeBriefRepository briefs {[this](const app::RunId& id) -> std::optional<app::OutcomeBriefRunOwner> {
        const auto found = owners.find(id);
        if (found == owners.end()) return std::nullopt;
        return found->second;
    }};
    StaticRuns runs;
    app::FollowUpPlanReadModel readModel;
    FollowUpPlanController controller;

    struct Finished {
        quint64 id;
        QString operation;
        QVariantMap result;
    };
    std::vector<Finished> finished;
    int statusChanges {0};
    int draftsCreatedSignals {0};
    QString lastGroup;

    explicit Rig(std::optional<ai::common::Error> refusal = std::nullopt)
        : h(fst::defaultFiles(), refusal)
        , readModel(briefs, h.requests, h.providers, h.models, h.drafts, runs, refusal)
        , controller(h.service, readModel)
    {
        QObject::connect(&controller, &FollowUpPlanController::operationFinished,
            [this](quint64 id, const QString& operation, const QVariantMap& result) { finished.push_back({id, operation, result}); });
        QObject::connect(&controller, &FollowUpPlanController::statusChanged, [this] { ++statusChanges; });
        QObject::connect(&controller, &FollowUpPlanController::draftsCreated, [this](const QString& group, int) {
            ++draftsCreatedSignals;
            lastGroup = group;
        });
        addRun(fst::kRun, app::RunStatus::Completed);
    }

    void addRun(const std::string& id, app::RunStatus status)
    {
        owners[id] = app::OutcomeBriefRunOwner {fst::kProject, status};
        runs.projects[id] = fst::kProject;
        require(briefs.append(briefFor(id, status), app::OutcomeBriefExpectedHead::none()).success(), "setup: a brief");
    }

    QVariantMap intent() const
    {
        return QVariantMap {
            {QStringLiteral("projectId"), q(fst::kProject)},
            {QStringLiteral("sourceRunId"), q(fst::kRun)},
            {QStringLiteral("kind"), QStringLiteral("next_run")},
            {QStringLiteral("goal"), QStringLiteral("Plan the next focused implementation step.")},
            {QStringLiteral("maxDrafts"), 2},
            {QStringLiteral("splitHint"), QStringLiteral("auto")},
            {QStringLiteral("providerId"), q(h.catalog.providerId)},
            {QStringLiteral("modelId"), q(h.catalog.modelId)}};
    }

    // Waits for the result of the operation `id` (the worker finishes; the signal comes on this thread).
    QVariantMap await(quint64 id)
    {
        require(id != 0, "the operation was accepted (an id, not 0)");
        const bool arrived = ai::ui::test_support::waitForCondition("the operation finishes", [&] {
            for (const auto& entry : finished) if (entry.id == id) return true;
            return false;
        }, 20s);
        require(arrived, "the operation finished");
        for (const auto& entry : finished) if (entry.id == id) return entry.result;
        return {};
    }

    QVariantMap statusRequest() const { return controller.status().value(QStringLiteral("request")).toMap(); }
    QString statusState() const { return statusRequest().value(QStringLiteral("state")).toString(); }
};

QString codeOf(const QVariantMap& map) { return map.value(QStringLiteral("errorCode")).toString(); }

void testNothingIsShownUntilARunWithABriefIsSelected()
{
    Rig rig;
    const QVariantMap empty = rig.controller.status();
    require(empty.value(QStringLiteral("availability")).toString() == QLatin1String("not_applicable"), "no Run: nothing to show");
    for (const char* key : {"runId", "projectId", "nextRun", "recovery", "requestPresent", "request", "errorCode", "errorMessage"}) {
        require(empty.contains(QString::fromLatin1(key)), std::string("the status map is complete even when empty: ") + key);
    }
    require(!rig.controller.busy() && rig.controller.lastError().isEmpty(), "idle and without an error");

    rig.controller.selectRun(QStringLiteral("no-such-run"));
    require(rig.controller.status().value(QStringLiteral("availability")).toString() == QLatin1String("not_applicable")
            && rig.controller.selectedRunId() == QLatin1String("no-such-run"),
        "a Run without a brief shows nothing");

    rig.controller.selectRun(q(fst::kRun));
    const QVariantMap status = rig.controller.status();
    require(status.value(QStringLiteral("availability")).toString() == QLatin1String("evaluated")
            && status.value(QStringLiteral("projectId")).toString() == q(fst::kProject),
        "a Completed outcome Run is evaluated");
    const QVariantMap nextRun = status.value(QStringLiteral("nextRun")).toMap();
    const QVariantMap recovery = status.value(QStringLiteral("recovery")).toMap();
    require(nextRun.value(QStringLiteral("offered")).toBool() && nextRun.value(QStringLiteral("allowed")).toBool()
            && nextRun.value(QStringLiteral("reasonCode")).toString().isEmpty(),
        "a next-Run plan is offered and allowed");
    require(!recovery.value(QStringLiteral("offered")).toBool() && !recovery.value(QStringLiteral("allowed")).toBool(),
        "a recovery plan is not offered for a Completed Run");
    require(!status.value(QStringLiteral("requestPresent")).toBool() && status.value(QStringLiteral("request")).toMap().isEmpty(), "no request yet");

    rig.addRun("run-failed", app::RunStatus::Failed);
    rig.controller.selectRun(QStringLiteral("run-failed"));
    const QVariantMap failed = rig.controller.status();
    require(failed.value(QStringLiteral("recovery")).toMap().value(QStringLiteral("allowed")).toBool()
            && !failed.value(QStringLiteral("nextRun")).toMap().value(QStringLiteral("offered")).toBool(),
        "a Failed Run is offered the recovery plan, with its own gate");

    const int before = rig.statusChanges;
    rig.controller.refresh();
    require(rig.statusChanges == before, "reading again without a change announces nothing");
}

void testThePickersAreTheBackendsOwn()
{
    Rig rig;
    const QVariantMap models = rig.controller.modelChoices();
    require(models.value(QStringLiteral("ok")).toBool(), "the model choices read");
    const QVariantList list = models.value(QStringLiteral("models")).toList();
    require(list.size() == 1, "the catalog's one model");
    const QVariantMap model = list.front().toMap();
    for (const char* key : {"providerId", "providerName", "providerKind", "modelId", "displayName", "usable", "reasonCode",
             "contextWindowTokens", "maxOutputTokens", "inputPriceMicrosPerMillionTokens", "outputPriceMicrosPerMillionTokens", "hasCredentialReference"}) {
        require(model.contains(QString::fromLatin1(key)), std::string("a model choice carries ") + key);
    }
    require(model.value(QStringLiteral("usable")).toBool() && model.value(QStringLiteral("modelId")).toString() == q(rig.h.catalog.modelId), "a priced model is usable");

    rig.h.editModel([](app::Model& m) { m.inputPriceMicrosPerMillionTokens = 0; });
    const QVariantMap unpriced = rig.controller.modelChoices().value(QStringLiteral("models")).toList().front().toMap();
    require(!unpriced.value(QStringLiteral("usable")).toBool() && unpriced.value(QStringLiteral("reasonCode")).toString() == QLatin1String("price_unknown"),
        "a model without a price is shown unusable with the reason");

    const QVariantMap templates = rig.controller.rosterTemplates(q(fst::kProject));
    require(templates.value(QStringLiteral("ok")).toBool() && templates.value(QStringLiteral("templates")).toList().isEmpty(), "no templates yet: an empty list, not a failure");
    rig.runs.choices.push_back(app::RunSourceChoice {"run-9", "Nine", app::RunStatus::Completed, fst::at(5)});
    const QVariantMap sources = rig.controller.sourceRunChoices(q(fst::kProject), {});
    require(sources.value(QStringLiteral("ok")).toBool() && sources.value(QStringLiteral("runs")).toList().size() == 1
            && sources.value(QStringLiteral("runs")).toList().front().toMap().value(QStringLiteral("status")).toString() == QLatin1String("completed"),
        "the source Run choices");
}

void testDescribeFilesIsAsynchronousAndCarriesMetadataOnly()
{
    Rig rig;
    const quint64 id = rig.controller.describeFiles(q(fst::kProject), q(fst::kRun), QStringLiteral("next_run"));
    require(id != 0 && rig.controller.busy() && rig.controller.busyOperation() == QLatin1String("describeFiles"), "accepted: the controller is busy");
    const quint64 second = rig.controller.describeFiles(q(fst::kProject), q(fst::kRun), QStringLiteral("next_run"));
    require(second == 0 && !rig.controller.lastError().isEmpty(), "ONE operation at a time: the second is refused and says why");
    const QVariantMap result = rig.await(id);
    require(result.value(QStringLiteral("ok")).toBool() && !rig.controller.busy(), "the files arrive and the controller is idle again");
    const QVariantList files = result.value(QStringLiteral("files")).toList();
    require(files.size() == 2, "both candidates are listed");
    const QVariantMap file = files.front().toMap();
    require(file.value(QStringLiteral("path")).toString() == QLatin1String("src/a.cpp") && file.value(QStringLiteral("eligible")).toBool()
            && file.value(QStringLiteral("includedByDefault")).toBool() && file.value(QStringLiteral("estimatedTokens")).toULongLong() > 0,
        "a file's path, eligibility and tokens");
    for (const QVariant& entry : files) {
        const QVariantMap map = entry.toMap();
        require(!map.contains(QStringLiteral("content")) && !map.contains(QStringLiteral("renderedText")) && map.value(QStringLiteral("sha256Short")).toString().size() == 12,
            "METADATA only: never a file's content, only a short digest");
    }
    require(rig.finished.size() == 1 && rig.finished.front().operation == QLatin1String("describeFiles"), "one finish was announced, named after its command");

    require(rig.controller.describeFiles(q(fst::kProject), q(fst::kRun), QStringLiteral("bogus")) == 0 && !rig.controller.lastError().isEmpty(),
        "an unknown kind is refused before anything starts");
    const quint64 refused = rig.controller.describeFiles(q(fst::kProject), QStringLiteral("run-not-a-run"), QStringLiteral("next_run"));
    const QVariantMap failure = rig.await(refused);
    require(!failure.value(QStringLiteral("ok")).toBool() && codeOf(failure).startsWith(QLatin1String("follow_up.")),
        "a Run the Backend cannot plan from is its own typed refusal: " + codeOf(failure).toStdString());
}

void testAQuoteIsFreeAndTheStatusLineFollowsIt()
{
    Rig rig;
    rig.controller.selectRun(q(fst::kRun));
    const quint64 id = rig.controller.requestQuote(rig.intent());
    const QVariantMap result = rig.await(id);
    require(result.value(QStringLiteral("ok")).toBool(), "the quote arrives: " + codeOf(result).toStdString());
    const QVariantMap request = result.value(QStringLiteral("request")).toMap();
    for (const char* key : {"id", "projectId", "sourceRunId", "kind", "state", "open", "terminal", "mayBeCharged", "goal", "maxDrafts", "splitHint",
             "createdAt", "quoteExpiresAt", "authorizedAt", "claimedAt", "finishedAt", "quote", "files", "failure", "accounting", "draftGroupId",
             "planFingerprint", "shortPlanFingerprint"}) {
        require(request.contains(QString::fromLatin1(key)), std::string("the request map carries ") + key);
    }
    require(request.value(QStringLiteral("state")).toString() == QLatin1String("quoted") && request.value(QStringLiteral("open")).toBool()
            && !request.value(QStringLiteral("terminal")).toBool() && !request.value(QStringLiteral("mayBeCharged")).toBool(),
        "a quoted request is open, not terminal and FREE");
    const QVariantMap quote = request.value(QStringLiteral("quote")).toMap();
    for (const char* key : {"providerId", "providerKind", "baseUrl", "modelId", "apiModelId", "promptCache", "estimatedInputTokens", "maxOutputTokens",
             "worstCaseCostMicros", "capMicros", "priceKnown", "contextFingerprint"}) {
        require(quote.contains(QString::fromLatin1(key)), std::string("the quote carries ") + key);
    }
    require(quote.value(QStringLiteral("estimatedInputTokens")).toULongLong() > 0 && quote.value(QStringLiteral("maxOutputTokens")).toULongLong() >= 4096
            && quote.value(QStringLiteral("worstCaseCostMicros")).toULongLong() > 0
            && quote.value(QStringLiteral("capMicros")).toULongLong() <= app::kFollowUpSystemCeilingMicros && quote.value(QStringLiteral("priceKnown")).toBool(),
        "the quote's tokens, worst case and cap are the Backend's, inside the ceiling");
    require(request.value(QStringLiteral("files")).toList().size() == 2 && result.value(QStringLiteral("files")).toList().size() == 2, "the pinned files and the whole candidate list");
    require(rig.h.client->calls() == 0 && rig.h.factory.requests == 0, "a quote sends NOTHING and never even asks for a provider client");

    // The status line shows the request the command made.
    require(rig.statusState() == QLatin1String("quoted") && rig.controller.status().value(QStringLiteral("requestPresent")).toBool(), "the status line shows the quoted request");
    const QVariantMap nextRun = rig.controller.status().value(QStringLiteral("nextRun")).toMap();
    require(!nextRun.value(QStringLiteral("allowed")).toBool() && nextRun.value(QStringLiteral("reasonCode")).toString() == QLatin1String("follow_up.request_open"),
        "...and the open request holds the Run's slot");

    // Quoting again replaces the quoted request in the same step.
    const QVariantMap again = rig.await(rig.controller.requestQuote(rig.intent()));
    require(again.value(QStringLiteral("replacedQuoteId")).toString() == request.value(QStringLiteral("id")).toString(), "a new quote reports the quote it replaced");
}

void testAnInvalidIntentIsRefusedBeforeAnythingStarts()
{
    Rig rig;
    const auto refuses = [&](const char* key, const QVariant& value, const char* label) {
        QVariantMap intent = rig.intent();
        intent.insert(QString::fromLatin1(key), value);
        const quint64 id = rig.controller.requestQuote(intent);
        require(id == 0 && !rig.controller.lastError().isEmpty() && !rig.controller.busy(), std::string("refused at the boundary: ") + label);
        rig.controller.clearError();
    };
    refuses("kind", QStringLiteral("everything"), "an unknown kind");
    refuses("splitHint", QStringLiteral("lots"), "an unknown split hint");
    refuses("maxDrafts", QStringLiteral("many"), "a draft count that is not a number");
    refuses("capMicros", QStringLiteral("cheap"), "a cap that is not a number");
    require(rig.h.service.findOpenForSourceRun(fst::kRun).value() == std::nullopt, "nothing was stored");

    // Values the Backend judges are passed through and come back as ITS typed refusal (never repaired here).
    QVariantMap tooMany = rig.intent();
    tooMany.insert(QStringLiteral("maxDrafts"), 9);
    const QVariantMap limits = rig.await(rig.controller.requestQuote(tooMany));
    require(!limits.value(QStringLiteral("ok")).toBool() && codeOf(limits) == QLatin1String("follow_up.invalid_draft_limits"), "9 drafts: the Backend's own refusal: " + codeOf(limits).toStdString());
    QVariantMap zeroCap = rig.intent();
    zeroCap.insert(QStringLiteral("capMicros"), 0);
    const QVariantMap cap = rig.await(rig.controller.requestQuote(zeroCap));
    require(!cap.value(QStringLiteral("ok")).toBool() && codeOf(cap) == QLatin1String("follow_up.invalid_cap"), "a zero cap is refused by the Backend, never turned into 'no cap': " + codeOf(cap).toStdString());
    QVariantMap lowCap = rig.intent();
    lowCap.insert(QStringLiteral("capMicros"), 100'000);
    const QVariantMap cheap = rig.await(rig.controller.requestQuote(lowCap));
    require(cheap.value(QStringLiteral("ok")).toBool()
            && cheap.value(QStringLiteral("request")).toMap().value(QStringLiteral("quote")).toMap().value(QStringLiteral("capMicros")).toULongLong() == 100'000u,
        "a lower cap reaches the quote");
    QVariantMap picked = rig.intent();
    picked.insert(QStringLiteral("selectedPaths"), QStringList {QStringLiteral("src/a.cpp")});
    const QVariantMap one = rig.await(rig.controller.requestQuote(picked));
    require(one.value(QStringLiteral("ok")).toBool() && one.value(QStringLiteral("request")).toMap().value(QStringLiteral("files")).toList().size() == 1,
        "an explicit file selection is exact");
    QVariantMap unknown = rig.intent();
    unknown.insert(QStringLiteral("selectedPaths"), QStringList {QStringLiteral("nope.cpp")});
    require(codeOf(rig.await(rig.controller.requestQuote(unknown))) == QLatin1String("follow_up.unknown_file"), "a file that is no candidate is refused by the Backend");
}

void testAuthorizeNudgesTheBackendAndThePollFollowsTheProcessor()
{
    Rig rig;
    rig.controller.selectRun(q(fst::kRun));
    const QVariantMap quoted = rig.await(rig.controller.requestQuote(rig.intent()));
    const QString id = quoted.value(QStringLiteral("request")).toMap().value(QStringLiteral("id")).toString();

    const QVariantMap authorized = rig.await(rig.controller.authorize(id));
    require(authorized.value(QStringLiteral("ok")).toBool(), "the authorisation arrives: " + codeOf(authorized).toStdString());
    require(authorized.value(QStringLiteral("request")).toMap().value(QStringLiteral("state")).toString() == QLatin1String("authorized"), "authorized");
    require(rig.h.nudged.size() == 1 && rig.h.nudged.front() == id.toStdString(), "the Backend's worker was nudged with the request id");
    require(rig.h.client->calls() == 0, "authorizing returns BEFORE any provider call: the controller never calls a provider");
    require(rig.statusState() == QLatin1String("authorized"), "the status line shows the authorised request");

    // The Backend's worker (here: the processor, driven by the test) makes the call; the controller polls until the request moves on.
    rig.h.client->push(fst::okReply(fst::replyOfPlanJson(fst::planJsonOf('c', 2, true))));
    const auto processed = rig.h.processor.processAuthorized(id.toStdString());
    require(processed.success() && processed.value().state == app::FollowUpPlanRequestState::PlanReady, "setup: the Backend composed the plan");
    const bool followed = ai::ui::test_support::waitForCondition("the status line reaches plan_ready", [&] { return rig.statusState() == QLatin1String("plan_ready"); }, 5s);
    require(followed, "the polled status line reaches plan_ready by itself");
    const QVariantMap ready = rig.statusRequest();
    require(ready.value(QStringLiteral("mayBeCharged")).toBool() && ready.value(QStringLiteral("accounting")).toMap().value(QStringLiteral("costMicros")).toULongLong() > 0
            && ready.value(QStringLiteral("planFingerprint")).toString().size() == 64,
        "a plan_ready request is possibly charged and carries what the call cost");
    const int changes = rig.statusChanges;
    for (const auto until = std::chrono::steady_clock::now() + 1500ms; std::chrono::steady_clock::now() < until;) ai::ui::test_support::pumpOneEventCycle();
    require(rig.statusChanges == changes, "once the request is settled nothing is polled or announced any more");
}

void testPreviewAndConfirmSaveTheStoredPlanOnce()
{
    Rig rig;
    const auto ready = rig.h.planReady();
    rig.controller.selectRun(q(fst::kRun));
    const QVariantMap preview = rig.controller.preview(q(ready.id));
    require(preview.value(QStringLiteral("ok")).toBool(), "the preview reads: " + codeOf(preview).toStdString());
    require(preview.value(QStringLiteral("origin")).toString() == QLatin1String("follow_up_request") && preview.value(QStringLiteral("previewToken")).toString() == q(ready.id)
            && preview.value(QStringLiteral("sourceRunId")).toString() == q(fst::kRun) && preview.value(QStringLiteral("existingGroupId")).toString().isEmpty(),
        "the preview has the Prompt Workshop's shape: the request id is its opaque token");
    for (const char* key : {"planTitle", "planFingerprint", "shortFingerprint", "sharedContractCount", "drafts", "warnings", "request", "messageId", "projectId"}) {
        require(preview.contains(QString::fromLatin1(key)), std::string("the preview carries ") + key);
    }
    const QVariantList drafts = preview.value(QStringLiteral("drafts")).toList();
    require(drafts.size() == 2, "both drafts of the stored plan");
    const QVariantMap draft = drafts.front().toMap();
    for (const char* key : {"key", "title", "task", "dependsOn", "sharedContractIds", "sharedContractCount", "contracts"}) {
        require(draft.contains(QString::fromLatin1(key)), std::string("a draft carries ") + key);
    }
    const QString fingerprint = preview.value(QStringLiteral("planFingerprint")).toString();
    require(fingerprint.toStdString() == ready.answer->planFingerprint, "the previewed plan is the stored one");

    // A refusal comes back typed and writes nothing.
    const QVariantMap wrong = rig.controller.confirm(q(ready.id), QString(64, QLatin1Char('0')));
    require(!wrong.value(QStringLiteral("ok")).toBool() && codeOf(wrong) == QLatin1String("follow_up.plan_changed") && rig.draftsCreatedSignals == 0,
        "another plan than the one shown is refused: " + codeOf(wrong).toStdString());
    const QVariantMap noTemplate = rig.controller.confirm(q(ready.id), fingerprint, QStringLiteral("no-such-template"));
    require(!noTemplate.value(QStringLiteral("ok")).toBool() && codeOf(noTemplate).startsWith(QLatin1String("follow_up.template_")), "an unknown roster template is refused");
    require(rig.statusState() == QLatin1String("plan_ready"), "...and the request is still plan_ready");

    const QVariantMap created = rig.controller.confirm(q(ready.id), fingerprint);
    require(created.value(QStringLiteral("ok")).toBool() && !created.value(QStringLiteral("alreadyExisted")).toBool(), "the explicit Confirm saves the group: " + codeOf(created).toStdString());
    require(created.value(QStringLiteral("origin")).toString() == QLatin1String("follow_up_request") && created.value(QStringLiteral("drafts")).toList().size() == 2
            && created.value(QStringLiteral("sourceRunIds")).toStringList() == QStringList {q(fst::kRun)},
        "two Drafts, the follow-up origin, the source Run is the group's");
    require(rig.draftsCreatedSignals == 1 && rig.lastGroup == created.value(QStringLiteral("groupId")).toString(), "the Drafts list is told exactly once");
    require(rig.statusState() == QLatin1String("drafts_saved") && rig.statusRequest().value(QStringLiteral("draftGroupId")).toString() == rig.lastGroup, "the status line says drafts_saved");

    const QVariantMap again = rig.controller.confirm(q(ready.id), fingerprint);
    require(again.value(QStringLiteral("ok")).toBool() && again.value(QStringLiteral("alreadyExisted")).toBool() && again.value(QStringLiteral("groupId")) == created.value(QStringLiteral("groupId"))
            && rig.draftsCreatedSignals == 1,
        "a repeated Confirm returns the SAME group and announces nothing");
    require(rig.controller.preview(q(ready.id)).value(QStringLiteral("existingGroupId")).toString() == rig.lastGroup, "a saved request previews with its group id");
}

void testAFailedRequestKeepsItsReplyReadOnly()
{
    Rig rig;
    const auto request = rig.h.authorized();
    rig.h.client->push(fst::okReply("This is not a plan at all."));
    const auto processed = rig.h.processor.processAuthorized(request.id);
    require(processed.success() && processed.value().state == app::FollowUpPlanRequestState::Failed, "setup: the answer was invalid");
    rig.controller.selectRun(q(fst::kRun));
    const QVariantMap map = rig.statusRequest();
    require(map.value(QStringLiteral("state")).toString() == QLatin1String("failed") && map.value(QStringLiteral("mayBeCharged")).toBool(), "a failed request that was claimed is possibly charged");
    const QVariantMap failure = map.value(QStringLiteral("failure")).toMap();
    require(!failure.value(QStringLiteral("code")).toString().isEmpty() && failure.value(QStringLiteral("hasStoredReply")).toBool(), "the failure carries the typed code and says a reply is stored");
    require(map.value(QStringLiteral("accounting")).toMap().value(QStringLiteral("inputTokens")).toULongLong() > 0, "what the answer cost is shown");
    const QVariantMap reply = rig.controller.storedReply(q(request.id));
    require(reply.value(QStringLiteral("ok")).toBool() && reply.value(QStringLiteral("reply")).toString() == QLatin1String("This is not a plan at all.")
            && reply.value(QStringLiteral("code")).toString() == failure.value(QStringLiteral("code")).toString(),
        "the stored reply, byte for byte, with the parser's code");
    const QVariantMap none = rig.controller.storedReply(q(rig.h.quoted().id));
    require(!none.value(QStringLiteral("ok")).toBool() && codeOf(none) == QLatin1String("follow_up.no_stored_reply"), "a request without an invalid answer has no stored reply");
    require(!rig.controller.preview(q(request.id)).value(QStringLiteral("ok")).toBool(), "a failed request has no plan to review");
}

void testCancelAndDiscardAreTheBackendsAndStayHonestAboutCost()
{
    Rig rig;
    rig.controller.selectRun(q(fst::kRun));
    const auto quoted = rig.h.quoted();
    const QVariantMap cancelled = rig.controller.cancel(q(quoted.id));
    require(cancelled.value(QStringLiteral("ok")).toBool() && cancelled.value(QStringLiteral("request")).toMap().value(QStringLiteral("state")).toString() == QLatin1String("cancelled")
            && !cancelled.value(QStringLiteral("request")).toMap().value(QStringLiteral("mayBeCharged")).toBool(),
        "cancelling a quote is free");
    require(rig.statusState() == QLatin1String("cancelled") && rig.controller.status().value(QStringLiteral("nextRun")).toMap().value(QStringLiteral("allowed")).toBool(),
        "the status line shows it and the Run's slot is free again");
    const QVariantMap twice = rig.controller.cancel(q(quoted.id));
    require(!twice.value(QStringLiteral("ok")).toBool() && !rig.controller.lastError().isEmpty(), "cancelling again is the Backend's typed refusal");

    // The Backend's clock is fixed in this rig: a later request needs a later instant, or two requests of one Run tie and only their
    // random ids would say which is the newest.
    rig.h.nowMs = 1000;
    const auto ready = rig.h.planReady();
    const QVariantMap discarded = rig.controller.discard(q(ready.id));
    require(discarded.value(QStringLiteral("ok")).toBool() && discarded.value(QStringLiteral("request")).toMap().value(QStringLiteral("state")).toString() == QLatin1String("discarded")
            && discarded.value(QStringLiteral("request")).toMap().value(QStringLiteral("mayBeCharged")).toBool(),
        "discarding a paid plan is NOT called free");
    require(rig.controller.status().value(QStringLiteral("requestPresent")).toBool() == false, "a discarded newest request shows nothing");
    require(!rig.controller.discard(QStringLiteral("no-such")).value(QStringLiteral("ok")).toBool(), "an unknown request is a typed failure");
}

void testAGuestRefusesEverythingBeforeItReadsOrWrites()
{
    const ai::common::Error refusal {"follow_up.not_the_owner", "guest"};
    Rig rig(refusal);
    rig.controller.selectRun(q(fst::kRun));
    const QVariantMap status = rig.controller.status();
    require(status.value(QStringLiteral("availability")).toString() == QLatin1String("unavailable")
            && status.value(QStringLiteral("errorCode")).toString() == QLatin1String("follow_up.not_the_owner"),
        "the status is a fault with the Backend's code, not an empty block");
    require(!rig.controller.modelChoices().value(QStringLiteral("ok")).toBool() && !rig.controller.rosterTemplates(q(fst::kProject)).value(QStringLiteral("ok")).toBool()
            && !rig.controller.sourceRunChoices(q(fst::kProject), {}).value(QStringLiteral("ok")).toBool(),
        "every picker refuses");
    require(codeOf(rig.await(rig.controller.describeFiles(q(fst::kProject), q(fst::kRun), QStringLiteral("next_run")))) == QLatin1String("follow_up.not_the_owner"), "describeFiles refuses");
    require(codeOf(rig.await(rig.controller.requestQuote(rig.intent()))) == QLatin1String("follow_up.not_the_owner"), "a quote refuses");
    require(codeOf(rig.await(rig.controller.authorize(QStringLiteral("any")))) == QLatin1String("follow_up.not_the_owner"), "an authorisation refuses");
    require(codeOf(rig.controller.cancel(QStringLiteral("any"))) == QLatin1String("follow_up.not_the_owner")
            && codeOf(rig.controller.discard(QStringLiteral("any"))) == QLatin1String("follow_up.not_the_owner")
            && codeOf(rig.controller.preview(QStringLiteral("any"))) == QLatin1String("follow_up.not_the_owner")
            && codeOf(rig.controller.confirm(QStringLiteral("any"), QStringLiteral("x"))) == QLatin1String("follow_up.not_the_owner"),
        "cancel, discard, preview and confirm refuse");
    require(rig.h.client->calls() == 0 && rig.h.nudged.empty() && rig.h.reader.reads() == 0, "nothing was read from a source, nudged or sent");
}

void testTheDestructorWaitsForARunningCall()
{
    Rig rig;
    auto lone = std::make_unique<FollowUpPlanController>(rig.h.service, rig.readModel);
    require(lone->describeFiles(q(fst::kProject), q(fst::kRun), QStringLiteral("next_run")) != 0, "a call is running");
    lone.reset();   // must wait for the worker, not crash or leak it
    require(true, "the controller was destroyed with a call in flight");
}

void testTheBridgeKeepsTheBlockOnTheSelectedRun()
{
    struct Workspace {
        fs::path root = fs::temp_directory_path() / ("aio-fupctl-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        fs::path projectPath = root / "project";
        fs::path databasePath = root / "state" / "orchestrator.sqlite";
        Workspace() { fs::create_directories(projectPath); }
        ~Workspace() { std::error_code ignored; fs::remove_all(root, ignored); }
    } workspace;
    const QString databasePath = QString::fromStdWString(workspace.databasePath.wstring());
    std::string projectId;
    {
        ai::backend::BackendRuntime first({workspace.databasePath});
        require(first.storageAvailable(), "Storage must be available");
        const auto created = first.projectService().createProject({"Follow-up project", "d", workspace.projectPath});
        require(created.success(), "The project must be created");
        projectId = created.value().id;
    }
    const std::string runId = "follow-run";
    {
        const auto opened = ai::infrastructure::SqliteDatabase::open(workspace.databasePath);
        require(opened.success(), "A raw connection must open");
        ai::infrastructure::SqliteRunRepository rawRuns(opened.value());
        ai::infrastructure::SqliteManifestWorkLedgerRepository rawLedger(opened.value());
        app::Run run;
        run.id = runId;
        run.projectId = projectId;
        run.title = "Follow";
        run.task = "A finished Manifest Run";
        run.workflowId = app::kExecutionManifestWorkflowId;
        run.status = app::RunStatus::Failed;
        run.createdAt = std::chrono::system_clock::now();
        require(rawRuns.insertRun(run).success(), "The Run must be seeded");
        app::ManifestWorkLedgerEntry entry;
        entry.runId = runId;
        entry.projectId = projectId;
        entry.terminalStatus = app::RunStatus::Failed;
        entry.disposition = app::ManifestWorkLedgerDisposition::NotProduced;
        entry.baselineFingerprint = std::string(64, 'b');
        entry.terminalReason = "The Run ended";
        require(rawLedger.insertEntry(entry).success(), "The ledger entry must be seeded");
    }

    auto bridge = std::make_unique<BackendBridge>(databasePath);
    require(bridge->storageAvailable(), "Storage must be available");
    FollowUpPlanController& followUp = bridge->followUpPlanController();
    require(static_cast<QObject*>(&followUp) == bridge->followUpPlan(), "QML reaches the same controller through backend.followUpPlan");
    require(followUp.selectedRunId().isEmpty() && followUp.status().value(QStringLiteral("availability")).toString() == QLatin1String("not_applicable"), "no Run is selected: no block");

    bridge->runController().selectRun(QString::fromStdString(runId));
    require(followUp.selectedRunId() == QString::fromStdString(runId), "the controller follows the application's selected Run at once");
    const bool evaluated = ai::ui::test_support::waitForCondition("the follow-up block evaluates the seeded Run", [&] {
        return followUp.status().value(QStringLiteral("availability")).toString() == QLatin1String("evaluated");
    }, 15s);
    require(evaluated, "once the Outcome Brief is written the follow-up block appears (it re-reads when the shown brief changes)");
    const QVariantMap recovery = followUp.status().value(QStringLiteral("recovery")).toMap();
    require(recovery.value(QStringLiteral("offered")).toBool() && recovery.value(QStringLiteral("allowed")).toBool(), "a Failed Run is offered recovery planning");

    // A real runtime's model picker and an unconfigured catalog: no usable model, no failure.
    const QVariantMap models = followUp.modelChoices();
    require(models.value(QStringLiteral("ok")).toBool(), "the picker reads through the real runtime");

    // A second bridge on the same file cannot open the storage the first one owns (a bridge has no guest mode: that is a test-fixture
    // option of BackendRuntime, covered by BackendRuntime's own tests). The follow-up surface must then FAIL CLOSED with the
    // runtime's controlled storage failure - a fault shown as a fault, never an empty block, never a crash.
    BackendBridge second(databasePath);
    require(!second.storageAvailable(), "the second bridge has no storage");
    second.followUpPlanController().selectRun(QString::fromStdString(runId));
    const QVariantMap secondStatus = second.followUpPlanController().status();
    require(secondStatus.value(QStringLiteral("availability")).toString() == QLatin1String("unavailable")
            && !secondStatus.value(QStringLiteral("errorCode")).toString().isEmpty(),
        "without storage the block is a fault with a code");
    require(!second.followUpPlanController().cancel(QStringLiteral("x")).value(QStringLiteral("ok")).toBool()
            && !second.followUpPlanController().modelChoices().value(QStringLiteral("ok")).toBool(),
        "...and every command fails closed");
}

} // namespace

int main(int argc, char* argv[])
{
    ai::ui::test_support::configureHeadlessFailureMode();
    QCoreApplication application(argc, argv);
    try {
        testNothingIsShownUntilARunWithABriefIsSelected();
        testThePickersAreTheBackendsOwn();
        testDescribeFilesIsAsynchronousAndCarriesMetadataOnly();
        testAQuoteIsFreeAndTheStatusLineFollowsIt();
        testAnInvalidIntentIsRefusedBeforeAnythingStarts();
        testAuthorizeNudgesTheBackendAndThePollFollowsTheProcessor();
        testPreviewAndConfirmSaveTheStoredPlanOnce();
        testAFailedRequestKeepsItsReplyReadOnly();
        testCancelAndDiscardAreTheBackendsAndStayHonestAboutCost();
        testAGuestRefusesEverythingBeforeItReadsOrWrites();
        testTheDestructorWaitsForARunningCall();
        testTheBridgeKeepsTheBlockOnTheSelectedRun();
        std::cout << "AIOrchestrator_FollowUpPlanController_tests passed\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << exception.what() << '\n';
        return 1;
    }
}
