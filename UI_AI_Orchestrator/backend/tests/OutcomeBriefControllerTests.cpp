#include "../../test_support/QtTestSupport.hpp"

#include <ai/application/InMemoryOutcomeBriefRepository.hpp>
#include <ai/application/InMemoryRepositories.hpp>
#include <ai/application/OutcomeBrief.hpp>
#include <ai/application/OutcomeBriefReadModelService.hpp>
#include <ai/application/OutcomeBriefReconciler.hpp>
#include <ai/backend/BackendRuntime.hpp>
#include <ai/infrastructure/SqliteRepositories.hpp>
#include <ai/ui/BackendBridge.hpp>
#include <ai/ui/OutcomeBriefController.hpp>

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
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>

// PC-4D: the Qt boundary of the read-only Outcome block. The controller only copies the Backend's OutcomeBriefView into a
// QVariantMap, so these tests pin (1) the shape and the values of that map for each availability, (2) when briefChanged() is and is not
// emitted, (3) the selection and the trailing re-read, and (4) the bridge wiring on a real BackendBridge / BackendRuntime over a
// disposable SQLite database. Nothing here reaches a provider, starts a Run, or reconciles by hand.

namespace {

namespace fs = std::filesystem;
namespace app = ai::application;
using namespace std::chrono_literals;

void require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

struct TempWorkspace {
    fs::path root = fs::temp_directory_path()
        / ("aio-obctl-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::path projectPath = root / "project";
    fs::path databasePath = root / "state" / "orchestrator.sqlite";

    TempWorkspace() { fs::create_directories(projectPath); }
    ~TempWorkspace()
    {
        std::error_code ignored;
        fs::remove_all(root, ignored);
    }
    [[nodiscard]] QString databasePathQt() const { return QString::fromStdWString(databasePath.wstring()); }
};

const std::string kProject = "project-1";

// The typed facts of a terminal Manifest Run: a Failed one without proposals, or a Completed one with one reviewable proposal.
app::OutcomeBriefFacts factsOf(const std::string& runId, const std::string& projectId, app::RunStatus status,
    const std::optional<std::string>& proposalId = std::nullopt)
{
    app::ManifestWorkLedgerEntry entry;
    entry.runId = runId;
    entry.projectId = projectId;
    entry.terminalStatus = status;
    entry.disposition = status != app::RunStatus::Completed ? app::ManifestWorkLedgerDisposition::NotProduced
                        : proposalId.has_value()           ? app::ManifestWorkLedgerDisposition::ManifestReady
                                                           : app::ManifestWorkLedgerDisposition::NoChangesNeeded;
    entry.baselineFingerprint = std::string(64, 'b');
    entry.terminalReason = "The Run ended";
    app::ManifestWorkLedgerTaskDisposition task;
    task.taskId = "task-1";
    task.status = "completed";
    app::OutcomeBriefFacts facts;
    if (proposalId.has_value()) {
        entry.proposalIds.push_back(*proposalId);
        app::ManifestWorkLedgerProposalLineage lineage;
        lineage.changeSetId = *proposalId;
        lineage.state = "ready_for_review";
        entry.proposalLineage.push_back(std::move(lineage));
        task.changeSetId = *proposalId;
        app::OutcomeBriefProposalFacts proposal;
        proposal.changeSetId = *proposalId;
        proposal.state = app::ChangeSetState::ReadyForReview;
        facts.proposals.push_back(std::move(proposal));
    }
    entry.taskDispositions.push_back(std::move(task));
    entry.affectedRelativePaths = {"src/a.cpp"};
    facts.runId = runId;
    facts.projectId = projectId;
    facts.workflowId = app::kExecutionManifestWorkflowId;
    facts.terminalStatus = status;
    facts.ledgerEntry = std::move(entry);
    return facts;
}

// A controller over in-memory repositories: a Run is there or not, a brief is stored or not, with no reconciler and no thread.
struct Rig {
    app::InMemoryRunRepository runs;
    app::InMemoryOutcomeBriefRepository briefs {[this](const app::RunId& runId) -> std::optional<app::OutcomeBriefRunOwner> {
        const auto found = runs.findRun(runId);
        if (!found.success() || !found.value().has_value()) return std::nullopt;
        return app::OutcomeBriefRunOwner {found.value()->projectId, found.value()->status};
    }};
    app::OutcomeBriefReadModelService service {runs, briefs};
    OutcomeBriefController controller {service};
    int changes {0};
    int ticks {0};

    Rig() { QObject::connect(&controller, &OutcomeBriefController::briefChanged, [this] { ++changes; }); }

    void addRun(const std::string& id, app::RunStatus status, const std::string& workflow = std::string(app::kExecutionManifestWorkflowId))
    {
        app::Run run;
        run.id = id;
        run.projectId = kProject;
        run.title = id;
        run.task = id;
        run.workflowId = workflow;
        run.status = status;
        run.createdAt = std::chrono::system_clock::time_point(std::chrono::seconds(1'700'000'000 + (++ticks)));
        require(runs.insertRun(run).success(), "setup: a Run");
    }

    void store(const app::OutcomeBriefFacts& facts)
    {
        const auto derived = app::deriveOutcomeBrief(facts);
        require(derived.success(), "setup: the facts derive a brief");
        require(briefs.append(derived.value(), app::OutcomeBriefExpectedHead::any()).success(), "setup: the brief is stored");
    }

    void setStatus(const std::string& id, app::RunStatus status)
    {
        auto run = runs.findRun(id);
        require(run.success() && run.value().has_value(), "setup: the Run to move on");
        run.value()->status = status;
        require(runs.updateRun(*run.value()).success(), "setup: the Run moves on");
    }

    [[nodiscard]] QString availability() const { return controller.brief().value(QStringLiteral("availability")).toString(); }
};

// Long enough for a stray trailing read (and for the real one to have come).
void pumpEventsForTheTrailingRead()
{
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(OutcomeBriefController::kTrailingRefreshMs + 600);
    while (std::chrono::steady_clock::now() < until) ai::ui::test_support::pumpOneEventCycle();
}

void testNothingIsShownUntilARunIsSelected()
{
    Rig rig;
    require(rig.controller.selectedRunId().isEmpty() && !rig.controller.applicable(), "no Run selected: nothing to show");
    require(rig.availability() == QLatin1String("not_applicable"), "...and the map says so");
    require(rig.controller.brief().contains(QStringLiteral("renderedText")) && rig.controller.brief().contains(QStringLiteral("counts")),
        "the map is complete even when empty (QML never meets a missing key)");

    rig.controller.selectRun(QString());
    rig.controller.refresh();
    require(rig.changes == 0, "selecting nothing and reading again change nothing: no signal");

    rig.addRun("mock", app::RunStatus::Completed, std::string(app::kMockWorkflowId));
    rig.controller.selectRun(QStringLiteral("mock"));
    require(rig.controller.selectedRunId() == QLatin1String("mock") && !rig.controller.applicable() && rig.availability() == QLatin1String("not_applicable"),
        "an ordinary Run is selected: still nothing to show");
    require(rig.changes == 1, "...but the selection itself is announced (selectedRunId is a property of the controller)");
    rig.controller.refresh();
    require(rig.changes == 1, "and reading it again announces nothing");
}

void testNotRecordedThenPresent()
{
    Rig rig;
    rig.addRun("run-1", app::RunStatus::Completed);
    rig.controller.selectRun(QStringLiteral("run-1"));
    require(rig.availability() == QLatin1String("not_recorded") && rig.controller.applicable() && rig.changes == 1,
        "a terminal outcome Run without a brief: the block says it is not recorded yet");
    require(rig.controller.brief().value(QStringLiteral("state")).toString().isEmpty()
            && rig.controller.brief().value(QStringLiteral("renderedText")).toString().isEmpty(),
        "...and shows no state and no text of its own");
    rig.controller.refresh();
    require(rig.changes == 1, "reading again without a change announces nothing");

    rig.store(factsOf("run-1", kProject, app::RunStatus::Completed, std::string("cs-1")));
    rig.controller.refresh();
    require(rig.changes == 2, "the brief appeared: announced once");
    const QVariantMap brief = rig.controller.brief();
    require(brief.value(QStringLiteral("availability")).toString() == QLatin1String("present"), "present");
    require(brief.value(QStringLiteral("runId")).toString() == QLatin1String("run-1"), "of this Run");
    require(brief.value(QStringLiteral("state")).toString() == QLatin1String("awaiting_review")
            && !brief.value(QStringLiteral("resolved")).toBool() && brief.value(QStringLiteral("pending")).toBool(),
        "awaiting review: pending, not resolved - as the Backend classified it");
    require(brief.value(QStringLiteral("terminalStatus")).toString() == QLatin1String("completed"), "the terminal status");
    require(brief.value(QStringLiteral("reasons")).toStringList().isEmpty(), "no reason");
    const QVariantMap counts = brief.value(QStringLiteral("counts")).toMap();
    require(counts.value(QStringLiteral("members")).toInt() == 1 && counts.value(QStringLiteral("reviewable")).toInt() == 1
            && counts.value(QStringLiteral("undecided")).toInt() == 1 && counts.value(QStringLiteral("applied")).toInt() == 0
            && counts.contains(QStringLiteral("humanRejected")) && counts.contains(QStringLiteral("approved"))
            && counts.contains(QStringLiteral("applyFailed")) && counts.contains(QStringLiteral("noChanges"))
            && counts.contains(QStringLiteral("validatorRejected")) && counts.contains(QStringLiteral("superseded"))
            && counts.contains(QStringLiteral("unsettled")) && counts.contains(QStringLiteral("inconsistent")),
        "all twelve typed counts, with the Backend's numbers");
    const QVariantList members = brief.value(QStringLiteral("members")).toList();
    require(members.size() == 1 && members.at(0).toMap().value(QStringLiteral("changeSetId")).toString() == QLatin1String("cs-1")
            && members.at(0).toMap().value(QStringLiteral("decision")).toString() == QLatin1String("undecided"),
        "the member and its typed decision");
    require(!brief.value(QStringLiteral("recoveryEligible")).toBool() && !brief.value(QStringLiteral("requiresInspection")).toBool(),
        "a Completed Run needs no recovery");
    require(brief.value(QStringLiteral("continuationSourceRunId")).toString().isEmpty()
            && brief.value(QStringLiteral("continuationChildRunId")).toString().isEmpty()
            && brief.value(QStringLiteral("manualHandoffId")).toString().isEmpty(),
        "no relation");
    require(brief.value(QStringLiteral("revision")).toULongLong() == 1u, "revision 1");
    require(brief.value(QStringLiteral("recordedAt")).toDateTime().isValid(), "recorded at a real time");
    require(brief.value(QStringLiteral("sourceStateFingerprint")).toString().size() == 64
            && brief.value(QStringLiteral("contentFingerprint")).toString().size() == 64,
        "both fingerprints, in full");
    const QString text = brief.value(QStringLiteral("renderedText")).toString();
    require(!text.isEmpty() && text.contains(QStringLiteral("cs-1")), "the stored text");

    // The proposal is applied and the reconciler writes the next revision: the new one is announced and fully replaces the old.
    rig.store(factsOf("run-1", kProject, app::RunStatus::Completed));
    rig.controller.refresh();
    require(rig.changes == 3 && rig.controller.brief().value(QStringLiteral("revision")).toULongLong() == 2u
            && rig.controller.brief().value(QStringLiteral("state")).toString() == QLatin1String("resolved_no_change")
            && rig.controller.brief().value(QStringLiteral("resolved")).toBool(),
        "the next revision replaces the current one and is announced");
}

void testSelectingAnotherRunLeavesNothingOfTheOldOne()
{
    Rig rig;
    rig.addRun("run-1", app::RunStatus::Failed);
    rig.addRun("run-2", app::RunStatus::Failed);
    rig.store(factsOf("run-1", kProject, app::RunStatus::Failed));
    rig.controller.selectRun(QStringLiteral("run-1"));
    require(rig.availability() == QLatin1String("present")
            && rig.controller.brief().value(QStringLiteral("state")).toString() == QLatin1String("recovery_only")
            && rig.controller.brief().value(QStringLiteral("recoveryEligible")).toBool(),
        "a Failed Run: recovery context only, eligible");

    rig.controller.selectRun(QStringLiteral("run-2"));
    require(rig.availability() == QLatin1String("not_recorded") && rig.controller.brief().value(QStringLiteral("runId")).toString() == QLatin1String("run-2")
            && rig.controller.brief().value(QStringLiteral("renderedText")).toString().isEmpty()
            && rig.controller.brief().value(QStringLiteral("sourceStateFingerprint")).toString().isEmpty(),
        "the next Run shows nothing of the previous one");

    rig.controller.selectRun(QString());
    require(rig.availability() == QLatin1String("not_applicable") && !rig.controller.applicable(), "no selection: no block");
}

void testAStaleBriefIsAFaultNotAnEmptyBlock()
{
    Rig rig;
    rig.addRun("run-1", app::RunStatus::Completed);
    rig.store(factsOf("run-1", kProject, app::RunStatus::Completed, std::string("cs-1")));
    rig.controller.selectRun(QStringLiteral("run-1"));
    require(rig.availability() == QLatin1String("present"), "setup: the brief is readable");

    auto run = rig.runs.findRun("run-1");
    require(run.success() && run.value().has_value(), "setup: the Run");
    run.value()->status = app::RunStatus::Failed;   // the Run left the status the brief describes
    require(rig.runs.updateRun(*run.value()).success(), "setup: the Run moves on");
    rig.controller.refresh();
    const QVariantMap brief = rig.controller.brief();
    require(brief.value(QStringLiteral("availability")).toString() == QLatin1String("unreadable")
            && brief.value(QStringLiteral("errorCode")).toString() == QLatin1String("persistence.invalid_data")
            && !brief.value(QStringLiteral("errorMessage")).toString().isEmpty(),
        "a head that no longer matches its Run is a fault with the Backend's code and message");
    require(brief.value(QStringLiteral("state")).toString().isEmpty() && brief.value(QStringLiteral("renderedText")).toString().isEmpty()
            && brief.value(QStringLiteral("revision")).toULongLong() == 0u && rig.controller.applicable(),
        "...and nothing of the stale brief is shown (the block itself stays, as a fault)");
}

void testScheduleRefreshReadsNowAndOnceMoreShortlyAfter()
{
    Rig rig;
    rig.controller.scheduleRefresh();
    require(rig.changes == 0, "no Run selected: nothing is read, nothing is armed");

    rig.addRun("run-1", app::RunStatus::Failed);
    rig.controller.selectRun(QStringLiteral("run-1"));
    require(rig.availability() == QLatin1String("not_recorded"), "setup: not recorded yet");

    // The event that can change the brief has happened (say the Run just finished), but the reconciler's own hook has not written yet:
    // the immediate read sees nothing...
    rig.controller.scheduleRefresh();
    require(rig.availability() == QLatin1String("not_recorded"), "the immediate read happens before the reconciler's hook");
    // ...and the hook writes a moment later.
    rig.store(factsOf("run-1", kProject, app::RunStatus::Failed));
    const bool arrived = ai::ui::test_support::waitForCondition("the trailing read", [&] { return rig.availability() == QLatin1String("present"); },
        [&] { return rig.availability().toStdString(); }, 5s);
    require(arrived, "the trailing read picks up the brief that was written after the event");

    // Selecting another Run drops what was armed for the previous one.
    rig.addRun("run-2", app::RunStatus::Failed);
    rig.controller.scheduleRefresh();
    rig.controller.selectRun(QStringLiteral("run-2"));
    const int before = rig.changes;
    rig.store(factsOf("run-2", kProject, app::RunStatus::Failed));
    pumpEventsForTheTrailingRead();
    require(rig.changes == before && rig.availability() == QLatin1String("not_recorded"),
        "the trailing read armed for the previous Run did not fire for the new one");
}

// The application's own selection does not touch the database on the spot: a Run that has just been started is selected while its worker
// is writing, and a synchronous SQLite read on the UI thread would wait for the worker.
void testTheApplicationsSelectionReadsLaterAndDropsTheOldBriefAtOnce()
{
    Rig rig;
    rig.addRun("run-1", app::RunStatus::Failed);
    rig.addRun("run-2", app::RunStatus::Failed);
    rig.store(factsOf("run-1", kProject, app::RunStatus::Failed));
    rig.store(factsOf("run-2", kProject, app::RunStatus::Failed));
    rig.controller.selectRun(QStringLiteral("run-1"));
    require(rig.availability() == QLatin1String("present"), "setup: the first brief is shown");

    const int before = rig.changes;
    rig.controller.selectRunSoon(QStringLiteral("run-2"));
    require(rig.controller.selectedRunId() == QLatin1String("run-2") && rig.availability() == QLatin1String("not_applicable")
            && rig.controller.brief().value(QStringLiteral("renderedText")).toString().isEmpty() && rig.changes == before + 1,
        "at once: the new Run is the selected one, nothing of the old brief is on screen, and nothing has been read yet");
    const bool arrived = ai::ui::test_support::waitForCondition("the deferred read of the selected Run", [&] {
        return rig.availability() == QLatin1String("present") && rig.controller.brief().value(QStringLiteral("runId")).toString() == QLatin1String("run-2");
    }, [&] { return rig.availability().toStdString(); }, 5s);
    require(arrived, "the read follows after the delay");

    const int settled = rig.changes;
    rig.controller.selectRunSoon(QStringLiteral("run-2"));
    require(rig.changes == settled, "selecting the Run that is already selected announces nothing");

    // A selection that changes again within the delay never reads at all.
    rig.controller.selectRunSoon(QStringLiteral("run-1"));
    rig.controller.selectRunSoon(QString());
    pumpEventsForTheTrailingRead();
    require(rig.controller.selectedRunId().isEmpty() && !rig.controller.applicable() && rig.availability() == QLatin1String("not_applicable"),
        "the Run that was deselected within the delay was never read");
}

// The whole shape: every field of the Backend's view arrives under its own key, with its own value (all values distinct, so a swap or a
// dropped field cannot hide), and the text survives as UTF-8.
void testEveryFieldOfTheViewReachesTheMapUnderItsOwnKey()
{
    app::OutcomeBriefView view;
    view.availability = app::OutcomeBriefView::Availability::Present;
    view.runId = "run-x";
    view.errorCode = "code-x";
    view.errorMessage = "message-x";
    view.state = "awaiting_apply";
    view.resolved = false;
    view.pending = true;
    view.terminalStatus = "completed";
    view.reasons = {"first", "second"};
    view.counts.members = 1;
    view.counts.reviewable = 2;
    view.counts.applied = 3;
    view.counts.humanRejected = 4;
    view.counts.approved = 5;
    view.counts.undecided = 6;
    view.counts.applyFailed = 7;
    view.counts.noChanges = 8;
    view.counts.validatorRejected = 9;
    view.counts.superseded = 10;
    view.counts.unsettled = 11;
    view.counts.inconsistent = 12;
    view.members = {{"cs-1", "applied"}, {"cs-2", "human_rejected"}};
    view.recoveryEligible = true;
    view.requiresInspection = true;
    view.continuationSourceRunId = std::string("source-x");
    view.continuationChildRunId = std::string("child-x");
    view.manualHandoffId = std::string("handoff-x");
    view.revision = 7;
    view.recordedAt = std::chrono::system_clock::time_point(std::chrono::milliseconds(1'700'000'000'123));
    view.sourceStateFingerprint = "source-fp";
    view.contentFingerprint = "content-fp";
    const std::string utf8Text = "line one\n\xD0\x9F\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82";   // "line one" and the Russian word "privet" in Cyrillic
    view.renderedText = utf8Text;

    const QVariantMap map = OutcomeBriefController::project(view);
    const auto text = [&](const char* key) { return map.value(QLatin1String(key)).toString(); };
    require(text("availability") == QLatin1String("present") && text("runId") == QLatin1String("run-x")
            && text("errorCode") == QLatin1String("code-x") && text("errorMessage") == QLatin1String("message-x"),
        "availability, Run and error");
    require(text("state") == QLatin1String("awaiting_apply") && !map.value(QStringLiteral("resolved")).toBool()
            && map.value(QStringLiteral("pending")).toBool() && text("terminalStatus") == QLatin1String("completed"),
        "state, flags and terminal status");
    require(map.value(QStringLiteral("reasons")).toStringList() == QStringList({QStringLiteral("first"), QStringLiteral("second")}),
        "the reasons, in order");
    const QVariantMap counts = map.value(QStringLiteral("counts")).toMap();
    const char* const names[] = {"members", "reviewable", "applied", "humanRejected", "approved", "undecided", "applyFailed",
        "noChanges", "validatorRejected", "superseded", "unsettled", "inconsistent"};
    require(counts.size() == 12, "exactly twelve counts");
    for (int index = 0; index < 12; ++index) {
        require(counts.value(QLatin1String(names[index])).toInt() == index + 1, names[index]);
    }
    const QVariantList members = map.value(QStringLiteral("members")).toList();
    require(members.size() == 2 && members.at(0).toMap().value(QStringLiteral("changeSetId")).toString() == QLatin1String("cs-1")
            && members.at(0).toMap().value(QStringLiteral("decision")).toString() == QLatin1String("applied")
            && members.at(1).toMap().value(QStringLiteral("changeSetId")).toString() == QLatin1String("cs-2")
            && members.at(1).toMap().value(QStringLiteral("decision")).toString() == QLatin1String("human_rejected"),
        "the members, in order, with their decisions");
    require(map.value(QStringLiteral("recoveryEligible")).toBool() && map.value(QStringLiteral("requiresInspection")).toBool(),
        "both recovery flags");
    require(text("continuationSourceRunId") == QLatin1String("source-x") && text("continuationChildRunId") == QLatin1String("child-x")
            && text("manualHandoffId") == QLatin1String("handoff-x"),
        "each relation under its own key");
    require(map.value(QStringLiteral("revision")).toULongLong() == 7u
            && map.value(QStringLiteral("recordedAt")).toDateTime().toMSecsSinceEpoch() == 1'700'000'000'123,
        "revision and the exact time");
    require(text("sourceStateFingerprint") == QLatin1String("source-fp") && text("contentFingerprint") == QLatin1String("content-fp"),
        "each fingerprint under its own key");
    require(map.value(QStringLiteral("renderedText")).toString() == QString::fromStdString(utf8Text) ||
            map.value(QStringLiteral("renderedText")).toString() == QString::fromUtf8(utf8Text.data(), static_cast<qsizetype>(utf8Text.size())),
        "the text, as UTF-8");
    require(map.value(QStringLiteral("renderedText")).toString().endsWith(QString::fromUtf16(u"\u041F\u0440\u0438\u0432\u0435\u0442")),
        "...and it really is the Cyrillic word, not mojibake");

    // An absent relation is an empty string, never a missing key; no revision means no time.
    const QVariantMap empty = OutcomeBriefController::project(app::OutcomeBriefView {});
    require(empty.contains(QStringLiteral("continuationSourceRunId")) && empty.value(QStringLiteral("continuationSourceRunId")).toString().isEmpty()
            && !empty.value(QStringLiteral("recordedAt")).isValid(),
        "an empty view: empty relations, no time");
}

// Reads happen only when they can matter: every one is a synchronous SQLite lookup on the UI thread, and a running Run publishes
// progress events all the time.
void testReadsHappenOnlyWhenTheyCanMatter()
{
    Rig rig;
    rig.addRun("live", app::RunStatus::Running);
    rig.controller.selectRun(QStringLiteral("live"));
    require(rig.availability() == QLatin1String("not_applicable") && !rig.controller.applicable(), "a running Run: no block");

    // A review / apply / handoff event announced while NO block is shown reads nothing and arms nothing: the Run finishes and gets its
    // brief behind the controller's back, and it is still not shown after the trailing window.
    rig.controller.scheduleRefresh();
    rig.setStatus("live", app::RunStatus::Failed);
    rig.store(factsOf("live", kProject, app::RunStatus::Failed));
    pumpEventsForTheTrailingRead();
    require(rig.availability() == QLatin1String("not_applicable"), "no block, so the event read nothing and armed no trailing read");

    // The finish of ANOTHER Run is none of its business.
    rig.controller.onRunFinished(QStringLiteral("someone-else"));
    rig.controller.onRunFinished(QString());
    require(rig.availability() == QLatin1String("not_applicable"), "the finish of another Run reads nothing");

    // The finish of the selected Run reads now: the Run has just become an outcome Run and the block appears.
    rig.controller.onRunFinished(QStringLiteral("live"));
    require(rig.availability() == QLatin1String("present"), "the finish of the selected Run reads at once");

    // ...and once more shortly after, because the reconciler's hook writes the brief a moment AFTER the finish.
    rig.addRun("live-2", app::RunStatus::Running);
    rig.controller.selectRun(QStringLiteral("live-2"));
    rig.setStatus("live-2", app::RunStatus::Failed);
    rig.controller.onRunFinished(QStringLiteral("live-2"));
    require(rig.availability() == QLatin1String("not_recorded"), "finished, but the brief is not written yet: the block says so");
    rig.store(factsOf("live-2", kProject, app::RunStatus::Failed));
    const bool arrived = ai::ui::test_support::waitForCondition("the trailing read after a finish",
        [&] { return rig.availability() == QLatin1String("present"); }, [&] { return rig.availability().toStdString(); }, 5s);
    require(arrived, "the trailing read picked up the brief that was written after the finish");
}

// ---- the bridge ---------------------------------------------------------------------------------------------------------------

void testTheBridgeKeepsTheBlockOnTheSelectedRun()
{
    TempWorkspace workspace;
    std::string projectId;
    {
        // The database is created and given a project by a runtime of its own, which is gone before the bridge opens the file.
        ai::backend::BackendRuntime first({workspace.databasePath});
        require(first.storageAvailable(), "Storage must be available");
        const auto created = first.projectService().createProject({"Outcome project", "d", workspace.projectPath});
        require(created.success(), "The project must be created");
        projectId = created.value().id;
    }
    const std::string runId = "outcome-run";
    {
        // An earlier process left a Failed Manifest Run with its Work Ledger entry (no verification: it never ran - a legitimate
        // early failure that reads back).
        const auto opened = ai::infrastructure::SqliteDatabase::open(workspace.databasePath);
        require(opened.success(), "A raw connection must open");
        ai::infrastructure::SqliteRunRepository rawRuns(opened.value());
        ai::infrastructure::SqliteManifestWorkLedgerRepository rawLedger(opened.value());
        app::Run run;
        run.id = runId;
        run.projectId = projectId;
        run.title = "Outcome";
        run.task = "A finished Manifest Run";
        run.workflowId = app::kExecutionManifestWorkflowId;
        run.status = app::RunStatus::Failed;
        run.createdAt = std::chrono::system_clock::now();
        require(rawRuns.insertRun(run).success(), "The Run must be seeded");
        app::Run other = run;
        other.id = "other-run";
        other.title = "Other";
        other.workflowId = app::kMockWorkflowId;
        other.status = app::RunStatus::Completed;
        other.createdAt = run.createdAt + 1s;
        require(rawRuns.insertRun(other).success(), "The ordinary Run must be seeded");
        auto facts = factsOf(runId, projectId, app::RunStatus::Failed);
        facts.ledgerEntry->verification.reset();
        facts.ledgerEntry->verificationSummary = {};
        require(rawLedger.insertEntry(*facts.ledgerEntry).success(), "The ledger entry must be seeded");
    }

    // The composition root is large; keep it off the test thread's stack like the other adapter tests.
    auto bridge = std::make_unique<BackendBridge>(workspace.databasePathQt());
    require(bridge->storageAvailable(), "Storage must be available");
    OutcomeBriefController& outcome = bridge->outcomeBriefController();
    require(static_cast<QObject*>(&outcome) == bridge->outcomeBrief(), "QML reaches the same controller through backend.outcomeBrief");
    require(outcome.selectedRunId().isEmpty() && !outcome.applicable(), "no Run is selected: no block");

    // Selection is the application's: the controller follows RunController, not whichever QML component happens to be visible.
    bridge->runController().selectRun(QString::fromStdString(runId));
    require(outcome.selectedRunId() == QString::fromStdString(runId), "the controller follows the selected Run at once");

    // The startup history pass (a worker thread) writes the brief; the controller reads what the Backend stored.
    const bool present = ai::ui::test_support::waitForCondition("the brief of the seeded Run", [&] {
        outcome.refresh();
        return outcome.brief().value(QStringLiteral("availability")).toString() == QLatin1String("present");
    }, [&] { return outcome.brief().value(QStringLiteral("availability")).toString().toStdString(); }, 10s);
    require(present, "the history pass wrote the brief and the controller shows it");
    const QVariantMap brief = outcome.brief();
    require(brief.value(QStringLiteral("state")).toString() == QLatin1String("recovery_only")
            && brief.value(QStringLiteral("terminalStatus")).toString() == QLatin1String("failed")
            && brief.value(QStringLiteral("recoveryEligible")).toBool() && !brief.value(QStringLiteral("requiresInspection")).toBool()
            && brief.value(QStringLiteral("revision")).toULongLong() == 1u,
        "a Failed Run: recovery context only, revision 1");
    const QString text = brief.value(QStringLiteral("renderedText")).toString();
    require(!text.isEmpty() && !text.contains(QString::fromStdWString(workspace.root.wstring())),
        "the stored text is there and carries no absolute path of this machine");

    // Switching to another Run. The other controllers announce state changes WHILE they switch (the agent session controller does so
    // synchronously) and the Outcome block listens to those announcements; it must therefore be on the NEW Run, and empty, before any
    // of them fires - otherwise the first announcement reads the PREVIOUS Run's brief with a synchronous SQLite lookup on the UI thread,
    // right while the worker of a Run that has just been started is writing. This slot is connected after the bridge's own, so it runs
    // after the Outcome controller has already reacted to the same announcement.
    // The agent session controller announces the start of a load synchronously only when it is idle; the load of the Run selected a
    // moment ago must have finished, or the switch below would announce nothing at all and the check would prove nothing.
    require(ai::ui::test_support::waitForCondition("the agent session controller is idle",
        [&] { return !bridge->agentSessionController().busy(); }, 10s),
        "setup: the load of the previously selected Run must have finished");
    int stateAnnouncements = 0;
    bool announcedWhileOnThePreviousRun = false;
    const QString otherRun = QStringLiteral("other-run");
    QObject::connect(&bridge->agentSessionController(), &AgentSessionController::stateChanged, &outcome, [&] {
        ++stateAnnouncements;
        if (outcome.selectedRunId() != otherRun || outcome.applicable()) announcedWhileOnThePreviousRun = true;
    });
    bridge->runController().selectRun(otherRun);
    require(stateAnnouncements >= 1, "setup: the agent session controller announced its state while the Run was switched");
    require(!announcedWhileOnThePreviousRun,
        "every state announcement made while switching found the Outcome block already on the new Run and empty, so none of them read the previous Run's brief");
    require(outcome.selectedRunId() == otherRun && !outcome.applicable()
            && outcome.brief().value(QStringLiteral("renderedText")).toString().isEmpty(),
        "and nothing of the previous Run's brief is on screen");

    bridge->runController().selectRun(QString());
    require(outcome.selectedRunId().isEmpty() && !outcome.applicable()
            && outcome.brief().value(QStringLiteral("renderedText")).toString().isEmpty(),
        "deselecting the Run removes the block and everything in it");
}

} // namespace

int main(int argc, char* argv[])
{
    ai::ui::test_support::configureHeadlessFailureMode();
    QCoreApplication application(argc, argv);
    try {
        testNothingIsShownUntilARunIsSelected();
        testNotRecordedThenPresent();
        testSelectingAnotherRunLeavesNothingOfTheOldOne();
        testAStaleBriefIsAFaultNotAnEmptyBlock();
        testScheduleRefreshReadsNowAndOnceMoreShortlyAfter();
        testReadsHappenOnlyWhenTheyCanMatter();
        testEveryFieldOfTheViewReachesTheMapUnderItsOwnKey();
        testTheApplicationsSelectionReadsLaterAndDropsTheOldBriefAtOnce();
        testTheBridgeKeepsTheBlockOnTheSelectedRun();
        std::cout << "AIOrchestrator_OutcomeBriefController_tests passed\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << exception.what() << '\n';
        return 1;
    }
}
