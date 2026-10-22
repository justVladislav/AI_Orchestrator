// Qt adapter tests for ComparisonController, ComparisonListModel,
// ComparisonResultListModel, and ComparisonProjectsModel.
// Uses in-memory repositories — no network, no SQLite, no real API calls.

#include <ai/application/ComparisonService.hpp>
#include <ai/application/IComparisonRepository.hpp>
#include <ai/application/ProjectService.hpp>
#include <ai/application/ProviderCatalogService.hpp>
#include <ai/ui/ComparisonController.hpp>
#include <ai/ui/ComparisonListModel.hpp>
#include <ai/ui/ComparisonProjectsModel.hpp>
#include <ai/ui/ComparisonResultListModel.hpp>

// Existing in-memory adapters for project / provider repositories
#include <ai/application/InMemoryRepositories.hpp>

#include <QAbstractItemModel>
#include <QCoreApplication>
#include <QObject>
#include <QSignalSpy>
#include <QString>
#include <QVariant>

#include <stdexcept>
#include <string>
#include <vector>

namespace {

using namespace ai::application;

void require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

// ── Test fixture ─────────────────────────────────────────────────────────────

struct Fixture {
    std::unique_ptr<IComparisonRepository> comparisonRepo;
    // Concrete in-memory implementations from InMemoryRepositories.hpp
    InMemoryProjectRepository  projectRepoImpl;
    InMemoryModelRepository    modelRepoImpl;
    InMemoryProviderRepository providerRepoImpl;

    ComparisonService      comparisonService;
    ProjectService         projectService;
    ProviderCatalogService catalogService;

    ComparisonController controller;

    int idCounter = 0;

    explicit Fixture()
        : comparisonRepo(makeInMemoryComparisonRepository())
        , comparisonService(*comparisonRepo, projectRepoImpl,
              [this] { return "id-" + std::to_string(++idCounter); })
        , projectService(projectRepoImpl)
        , catalogService(providerRepoImpl, modelRepoImpl)
        , controller(comparisonService, projectService, catalogService)
    {
    }

    Project addProject(const std::string& id, const std::string& name)
    {
        Project p;
        p.id = id;
        p.name = name;
        p.rootPath = std::filesystem::temp_directory_path() / name;
        projectRepoImpl.insert(p);
        return p;
    }

    ComparisonSession createSession(const std::string& projectId, const std::string& title)
    {
        CreateComparisonRequest req;
        req.projectId = projectId;
        req.title = title;
        req.task = "task";
        ComparisonProviderSlot s;
        s.providerId = "openai";
        s.providerName = "OpenAI";
        s.modelId = "gpt-4o";
        s.modelName = "GPT-4o";
        req.slots = {s};
        auto res = comparisonService.createSession(req);
        if (!res.success()) throw std::runtime_error("createSession failed");
        return res.value();
    }
};

// ── Test 1: projectsModel is populated on construction ───────────────────────

void test_projectsModel_populatedOnConstruction()
{
    Fixture f;
    f.addProject("p1", "Alpha");
    f.addProject("p2", "Beta");
    // ComparisonController calls loadProjects() in its constructor.
    // But projects were added after construction, so reload via refresh().
    f.controller.refresh();

    QAbstractItemModel* model = f.controller.projectsModel();
    require(model != nullptr, "projectsModel must not be null");
    require(model->rowCount() == 2, "projectsModel must contain 2 rows");

    const QVariant itemId = model->data(model->index(0, 0), ComparisonProjectsModel::ItemIdRole);
    require(!itemId.toString().isEmpty(), "itemId role must not be empty");

    const QVariant itemLabel = model->data(model->index(0, 0), ComparisonProjectsModel::ItemLabelRole);
    require(!itemLabel.toString().isEmpty(), "itemLabel role must not be empty");
}

// ── Test 2: selectProject loads comparison list model ────────────────────────

void test_selectProject_loadsComparisonListModel()
{
    Fixture f;
    f.addProject("p1", "Alpha");
    f.createSession("p1", "Comparison A");
    f.createSession("p1", "Comparison B");
    f.controller.refresh();

    f.controller.selectProject(QStringLiteral("p1"));

    QAbstractItemModel* model = f.controller.comparisonListModel();
    require(model->rowCount() == 2, "must load 2 comparisons for p1");

    // itemId / itemLabel roles must be present (needed by ComparisonComboBox)
    const QVariant itemId = model->data(model->index(0, 0), ComparisonListModel::ItemIdRole);
    const QVariant itemLabel = model->data(model->index(0, 0), ComparisonListModel::ItemLabelRole);
    require(!itemId.toString().isEmpty(), "itemId role must not be empty");
    require(!itemLabel.toString().isEmpty(), "itemLabel role must not be empty");
}

// ── Test 3: selectComparison loads result list model ─────────────────────────

void test_selectComparison_loadsResultListModel()
{
    Fixture f;
    f.addProject("p1", "Alpha");
    const auto session = f.createSession("p1", "Comparison A");
    f.controller.refresh();
    f.controller.selectProject(QStringLiteral("p1"));

    QSignalSpy spy(&f.controller, &ComparisonController::resultsUpdated);
    f.controller.selectComparison(QString::fromStdString(session.id));

    require(f.controller.resultListModel()->rowCount() == 1, "result list must have 1 row");
    require(!spy.isEmpty(), "resultsUpdated must be emitted");
}

// ── Test 4: markAsBest emits resultsUpdated ───────────────────────────────────

void test_markAsBest_emitsResultsUpdated()
{
    Fixture f;
    f.addProject("p1", "Alpha");
    const auto session = f.createSession("p1", "Test");
    f.controller.refresh();
    f.controller.selectProject(QStringLiteral("p1"));
    f.controller.selectComparison(QString::fromStdString(session.id));

    const auto results = f.comparisonService.listResultsBySession(session.id).value();
    const QString resultId = QString::fromStdString(results[0].id);

    QSignalSpy spy(&f.controller, &ComparisonController::resultsUpdated);
    f.controller.markAsBest(QString::fromStdString(session.id), resultId);

    require(!spy.isEmpty(), "resultsUpdated must be emitted after markAsBest");
}

// ── Test 5: resultsSnapshot returns correct QVariantList ─────────────────────

void test_resultsSnapshot_returnsCorrectData()
{
    Fixture f;
    f.addProject("p1", "Alpha");
    const auto session = f.createSession("p1", "Test");
    f.controller.refresh();
    f.controller.selectProject(QStringLiteral("p1"));
    f.controller.selectComparison(QString::fromStdString(session.id));

    const QVariantList snapshot = f.controller.resultsSnapshot();
    require(snapshot.size() == 1, "snapshot must have 1 entry");

    const QVariantMap entry = snapshot[0].toMap();
    require(entry.contains(QStringLiteral("resultId")), "snapshot must contain resultId");
    require(entry.contains(QStringLiteral("status")), "snapshot must contain status");
    require(entry[QStringLiteral("status")].toString() == QStringLiteral("pending"),
            "default status must be pending");
}

// ── Test 6: selectedComparisonUpdatedAt returns non-empty after selection ─────

void test_selectedComparisonUpdatedAt_returnsNonEmptyString()
{
    Fixture f;
    f.addProject("p1", "Alpha");
    const auto session = f.createSession("p1", "Test");
    f.controller.refresh();
    f.controller.selectProject(QStringLiteral("p1"));
    f.controller.selectComparison(QString::fromStdString(session.id));

    const QString updatedAt = f.controller.selectedComparisonUpdatedAt();
    require(!updatedAt.isEmpty(), "selectedComparisonUpdatedAt must not be empty after selection");
}

// ── Test 7: selectProject clears resultListModel for a different project ──────

void test_selectProject_clearsResultListOnSwitch()
{
    Fixture f;
    f.addProject("p1", "Alpha");
    f.addProject("p2", "Beta");
    const auto session = f.createSession("p1", "Test");
    f.controller.refresh();
    f.controller.selectProject(QStringLiteral("p1"));
    f.controller.selectComparison(QString::fromStdString(session.id));
    require(f.controller.resultListModel()->rowCount() == 1, "must have 1 result for p1 session");

    f.controller.selectProject(QStringLiteral("p2"));
    require(f.controller.resultListModel()->rowCount() == 0, "result model must be cleared on project switch");
}

// ── Test 8: ComparisonProjectsModel exposes itemId and itemLabel roles ────────

void test_comparisonProjectsModel_exposesRequiredRoles()
{
    ComparisonProjectsModel model;
    Project p;
    p.id = "pid";
    p.name = "Project Name";
    p.rootPath = "/some/path";
    model.replace({p});

    require(model.rowCount() == 1, "model must have 1 row");

    const QVariant itemId = model.data(model.index(0), ComparisonProjectsModel::ItemIdRole);
    const QVariant itemLabel = model.data(model.index(0), ComparisonProjectsModel::ItemLabelRole);
    const QVariant projectId = model.data(model.index(0), ComparisonProjectsModel::ProjectIdRole);
    const QVariant name = model.data(model.index(0), ComparisonProjectsModel::NameRole);

    require(itemId.toString() == QStringLiteral("pid"), "itemId must equal project id");
    require(itemLabel.toString() == QStringLiteral("Project Name"), "itemLabel must equal project name");
    require(projectId.toString() == itemId.toString(), "projectId role must alias itemId");
    require(name.toString() == itemLabel.toString(), "name role must alias itemLabel");
}

} // namespace

int main(int argc, char* argv[])
{
    QCoreApplication app(argc, argv);

    test_projectsModel_populatedOnConstruction();
    test_selectProject_loadsComparisonListModel();
    test_selectComparison_loadsResultListModel();
    test_markAsBest_emitsResultsUpdated();
    test_resultsSnapshot_returnsCorrectData();
    test_selectedComparisonUpdatedAt_returnsNonEmptyString();
    test_selectProject_clearsResultListOnSwitch();
    test_comparisonProjectsModel_exposesRequiredRoles();

    return 0;
}
