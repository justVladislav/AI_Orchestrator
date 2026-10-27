#include "ai/application/workspace/FilesystemPathUtils.hpp"
#include "ai/application/ProjectReadinessService.hpp"
#include "ai/infrastructure/SqliteRepositories.hpp"

#include <sqlite3.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;
using namespace ai::application;

void require(bool condition, const std::string& message)
{
    // Active in Release builds, unlike assert().
    if (!condition) throw std::runtime_error(message);
}

struct TestDirectory {
    fs::path path;

    explicit TestDirectory(const std::string& tag)
    {
        static int counter = 0;
        path = fs::temp_directory_path()
            / ("aio-pr-" + tag + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count() % 1000000)
                  + "-" + std::to_string(++counter));
        std::error_code error;
        fs::create_directories(path, error);
        require(!error, "could not create the test directory");
    }
    ~TestDirectory()
    {
        std::error_code ignored;
        fs::remove_all(detail::longPath(path), ignored);
    }
    TestDirectory(const TestDirectory&) = delete;
    TestDirectory& operator=(const TestDirectory&) = delete;
};

void writeFile(const fs::path& path, const std::string& content)
{
    std::error_code error;
    fs::create_directories(path.parent_path(), error);
    std::ofstream(path, std::ios::binary) << content;
}

// ---- a scripted cmake (same contract as the readiness-check tests) ----

struct FakeCmake final : IManifestVerificationCommandRunner {
    std::vector<std::vector<std::string>> calls;
    std::string configureListing {"Available configure presets:\n\n  \"local\" - Local\n"};
    std::string buildListing {"Available build presets:\n\n  \"local-build\" - Local build\n"};
    int configureExit {0};
    std::string configureOutput {"-- Configuring done\n"};
    bool configureCancelled {false};
    std::string binaryDir {"build/local"};
    std::function<void()> onConfigure;
    fs::path root;

    ManifestVerificationCommandOutcome run(const ManifestVerificationCommandRequest& request, std::stop_token) override
    {
        calls.push_back(request.arguments);
        ManifestVerificationCommandOutcome outcome;
        outcome.started = true;
        outcome.exitCode = 0;
        const auto& args = request.arguments;
        if (!args.empty() && args[0] == "--list-presets=configure") {
            outcome.boundedOutput = configureListing;
        } else if (!args.empty() && args[0] == "--list-presets=build") {
            outcome.boundedOutput = buildListing;
        } else if (args.size() == 2 && args[0] == "--preset") {
            if (onConfigure) onConfigure();
            if (configureCancelled) {
                outcome.cancelled = true;
                return outcome;
            }
            outcome.exitCode = configureExit;
            outcome.boundedOutput = configureOutput;
            if (configureExit == 0) {
                writeFile(root / binaryDir / "CMakeCache.txt",
                    "CMAKE_CXX_COMPILER:FILEPATH=C:/VS/VC/Tools/MSVC/14.51/bin/Hostx64/x64/cl.exe\n");
                // The configure writes its own cmake files into the build tree; they must never count as inputs.
                writeFile(root / binaryDir / "cmake_install.cmake", "# generated " + std::to_string(calls.size()));
                writeFile(root / binaryDir / "CMakeFiles" / "generated.cmake", "# generated " + std::to_string(calls.size()));
                writeFile(root / binaryDir / "CMakeFiles" / "CMakeLists.txt", "# generated " + std::to_string(calls.size()));
            }
        } else if (!args.empty() && args[0] == "--build") {
            outcome.boundedOutput = "The following are some of the valid targets for this Makefile:\n... all (the default if no "
                                    "target is provided)\n... app\n... app_qmllint\n";
        } else {
            outcome.exitCode = 99;
        }
        return outcome;
    }

    [[nodiscard]] std::size_t configureCalls() const
    {
        return static_cast<std::size_t>(std::count_if(calls.begin(), calls.end(),
            [](const auto& call) { return call.size() == 2 && call[0] == "--preset"; }));
    }
    [[nodiscard]] std::size_t allCalls() const { return calls.size(); }
};

const char* kPreset = R"json({
  "version": 3,
  "configurePresets": [
    {"name": "local", "generator": "Ninja", "binaryDir": "${sourceDir}/build/${presetName}",
     "cacheVariables": {"CMAKE_PREFIX_PATH": "C:/Qt/6.11.1/msvc2022_64"}}
  ],
  "buildPresets": [{"name": "local-build", "configurePreset": "local"}]
})json";

void makeProject(const fs::path& root)
{
    writeFile(root / "CMakePresets.json", kPreset);
    writeFile(root / "CMakeLists.txt", "cmake_minimum_required(VERSION 3.20)\nproject(app CXX)\n");
    writeFile(root / "cmake" / "helpers.cmake", "# helpers v1\n");
    writeFile(root / "main.cpp", "int main() { return 0; }\n");
    writeFile(root / "Main.qml", "Item {}\n");
}

struct Harness {
    InMemoryProjectReadinessRepository repository;
    std::string toolchainIdentity {"msvc=14.51;sdk=10.0.26100.0;vs=aaaa"};
    bool toolchainPrepared {true};

    ProjectSetupReadiness::ToolchainProvider provider()
    {
        return [this] {
            VerificationToolchainEnvironment environment;
            environment.prepared = toolchainPrepared;
            environment.identity = toolchainPrepared ? toolchainIdentity : std::string();
            if (!toolchainPrepared) {
                environment.errorCode = kToolchainWindowsSdkMissing;
                environment.message = "Install a Windows SDK and check again.";
            }
            return environment;
        };
    }
    ProjectReadinessService service() { return ProjectReadinessService(repository, provider()); }
};

ProjectSetupCheck ensure(ProjectReadinessService& service, FakeCmake& cmake, const ProjectId& id, const fs::path& root,
    bool force = false, std::stop_token stop = {})
{
    cmake.root = root;
    return service.ensureReady(id, root, cmake, stop, force);
}

// ---- tests ----

void testTheFingerprintTracksConfigurationInputsAndNothingElse()
{
    TestDirectory project("fp");
    makeProject(project.path);
    writeFile(project.path / "build" / "local" / "CMakeCache.txt", "x");        // a build tree
    writeFile(project.path / "build" / "local" / "cmake_install.cmake", "generated"); // generated in the tree root
    writeFile(project.path / "build" / "local" / "CMakeFiles" / "a.cmake", "x");  // generated, inside it
    writeFile(project.path / "node_modules" / "p" / "CMakeLists.txt", "x");        // never an input
    const auto base = ProjectReadinessService::computeInputsFingerprint(project.path);
    require(base.success() && base.value().size() == 64, "A fingerprint is computed");
    require(ProjectReadinessService::computeInputsFingerprint(project.path).value() == base.value(), "...and is stable");

    writeFile(project.path / "build" / "local" / "CMakeFiles" / "a.cmake", "changed by the build");
    writeFile(project.path / "build" / "local" / "cmake_install.cmake", "regenerated");
    writeFile(project.path / "build" / "local" / "CMakeFiles" / "new" / "CMakeLists.txt", "new");
    writeFile(project.path / "node_modules" / "p" / "CMakeLists.txt", "changed");
    writeFile(project.path / "main.cpp", "int main() { return 1; }\n");
    writeFile(project.path / "Main.qml", "Item { id: changed }\n");
    writeFile(project.path / "notes.txt", "a new, unrelated file");
    require(ProjectReadinessService::computeInputsFingerprint(project.path).value() == base.value(),
        "Generated build files, dependencies and source edits (a QML title) are not inputs");

    const auto changes = std::vector<std::pair<std::string, fs::path>> {
        {"CMakeLists.txt", project.path / "CMakeLists.txt"},
        {"a *.cmake module", project.path / "cmake" / "helpers.cmake"},
        {"a preset file", project.path / "CMakePresets.json"},
        {"a sub-project CMakeLists.txt", project.path / "sub" / "CMakeLists.txt"},
        {"a dependency manifest", project.path / "vcpkg.json"},
    };
    for (const auto& [label, path] : changes) {
        std::string previous;
        {
            std::ifstream in(path, std::ios::binary);
            previous.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        }
        writeFile(path, previous + "\n# edited");
        require(ProjectReadinessService::computeInputsFingerprint(project.path).value() != base.value(),
            "Editing " + label + " changes the fingerprint");
        if (previous.empty()) {
            std::error_code ignored;
            fs::remove(path, ignored);
        } else {
            writeFile(path, previous);
        }
        require(ProjectReadinessService::computeInputsFingerprint(project.path).value() == base.value(),
            "...and restoring it restores the fingerprint (" + label + ")");
    }

    // An included preset file and a toolchain file named by a preset are inputs too.
    writeFile(project.path / "CMakePresets.json", R"json({
      "version": 4, "include": ["extra.json"],
      "configurePresets": [{"name": "local", "generator": "Ninja", "binaryDir": "${sourceDir}/build/${presetName}",
                            "toolchainFile": "${sourceDir}/tools/tc.txt"}],
      "buildPresets": [{"name": "local-build", "configurePreset": "local"}]})json");
    writeFile(project.path / "extra.json", "{\"version\": 4}");
    writeFile(project.path / "tools" / "tc.txt", "toolchain v1");
    const auto withIncludes = ProjectReadinessService::computeInputsFingerprint(project.path).value();
    writeFile(project.path / "extra.json", "{\"version\": 4, \"configurePresets\": []}");
    const auto afterInclude = ProjectReadinessService::computeInputsFingerprint(project.path).value();
    require(afterInclude != withIncludes, "A change in an included preset file is a change");
    writeFile(project.path / "tools" / "tc.txt", "toolchain v2");
    require(ProjectReadinessService::computeInputsFingerprint(project.path).value() != afterInclude,
        "A change in the preset's toolchain file is a change");

    require(!ProjectReadinessService::computeInputsFingerprint(project.path / "missing").success(),
        "A missing project folder has no fingerprint");

    // The stored fingerprint also names the pair: the same inputs under another pair are another setup.
    const auto inputs = ProjectReadinessService::computeInputsFingerprint(project.path).value();
    require(ProjectReadinessService::fingerprintFor(inputs, {"debug", "debug-build"})
                != ProjectReadinessService::fingerprintFor(inputs, {"release", "release-build"}),
        "Another configure/build pair is another fingerprint");
}

void testASuccessfulSetupIsRecordedSurvivesARestartAndIsNotRepeated()
{
    TestDirectory base("once");
    // A non-ASCII folder name (written only as \u escapes): the record must find the same project again.
    const fs::path project = base.path / fs::path(u8"\u043f\u0430\u043f\u043a\u0430-\u6e2c\u8a66");
    makeProject(project);
    Harness harness;
    FakeCmake cmake;

    auto service = harness.service();
    auto first = ensure(service, cmake, "p1", project);
    require(first.ready, "The first setup passes: " + first.failureMessage);
    require(cmake.configureCalls() == 1, "...with one real configure");
    const auto recorded = harness.repository.find("p1").value();
    require(recorded.has_value() && recorded->state == ProjectReadinessState::Ready, "Readiness is recorded as ready");
    require(recorded->configurePreset == "local" && recorded->buildPreset == "local-build", "...for the chosen pair");
    require(recorded->binaryDir == "build/local", "...with the resolved build directory (${presetName} expanded)");
    require(recorded->toolchainIdentity == harness.toolchainIdentity && !recorded->inputsFingerprint.empty(),
        "...with the toolchain and the inputs it was gathered with");
    require(recorded->lastSuccessAt.has_value() && !recorded->targets.empty() && recorded->targetsComplete,
        "...and the confirmed targets");

    const auto callsAfterFirst = cmake.allCalls();
    auto second = ensure(service, cmake, "p1", project);
    require(second.ready && second.targets == first.targets && second.selection == first.selection,
        "An unchanged project is ready again, with the recorded evidence");
    require(cmake.allCalls() == callsAfterFirst, "...without starting cmake at all");

    // A restart: a new service over the same durable record.
    auto restarted = harness.service();
    FakeCmake afterRestart;
    const auto third = ensure(restarted, afterRestart, "p1", project);
    require(third.ready && afterRestart.allCalls() == 0, "The state survives a restart and nothing is configured again");
}

void testSourceEditsKeepTheStateButConfigurationEditsInvalidateIt()
{
    TestDirectory base("edit");
    const fs::path project = base.path / "p";
    makeProject(project);
    Harness harness;
    FakeCmake cmake;
    auto service = harness.service();
    require(ensure(service, cmake, "p1", project).ready, "first setup");
    const auto configures = cmake.configureCalls();

    writeFile(project / "Main.qml", "Item { Text { text: \"A new title\" } }\n");
    writeFile(project / "main.cpp", "int main() { return 2; }\n");
    require(ensure(service, cmake, "p1", project).ready && cmake.configureCalls() == configures,
        "Changing a QML title or a source file does not invalidate the setup");

    writeFile(project / "CMakeLists.txt", "cmake_minimum_required(VERSION 3.20)\nproject(app CXX)\nadd_subdirectory(x)\n");
    const auto assessed = service.assess("p1", project);
    require(assessed.success() && !assessed.value().fresh && assessed.value().reason == "configuration_changed",
        "A CMakeLists.txt edit is detected without running cmake");
    require(harness.repository.find("p1").value()->state == ProjectReadinessState::Stale, "...and recorded as stale");
    require(ensure(service, cmake, "p1", project).ready && cmake.configureCalls() == configures + 1,
        "...so the next Run proves the project again");

    writeFile(project / "CMakePresets.json", std::string(kPreset) + "\n");
    require(!service.assess("p1", project).value().fresh, "A preset file edit invalidates the setup");
    require(ensure(service, cmake, "p1", project).ready, "...and it passes again");
}

void testAChangedToolchainRootOrBuildTreeNeedsANewProof()
{
    TestDirectory base("drift");
    const fs::path project = base.path / "p";
    makeProject(project);
    Harness harness;
    FakeCmake cmake;
    auto service = harness.service();
    require(ensure(service, cmake, "p1", project).ready, "first setup");

    harness.toolchainIdentity = "msvc=14.51;sdk=10.0.26200.0;vs=aaaa"; // a newer SDK
    auto assessed = service.assess("p1", project);
    require(!assessed.value().fresh && assessed.value().reason == "toolchain_changed", "A changed SDK invalidates: " + assessed.value().reason);
    const auto configures = cmake.configureCalls();
    require(ensure(service, cmake, "p1", project).ready && cmake.configureCalls() == configures + 1, "...and is proven again");
    require(harness.repository.find("p1").value()->toolchainIdentity == harness.toolchainIdentity,
        "The new record names the new toolchain");

    harness.toolchainPrepared = false;
    assessed = service.assess("p1", project);
    require(!assessed.value().fresh && assessed.value().reason == "toolchain_unavailable", "A toolchain that cannot be prepared is not ready");
    const auto failed = ensure(service, cmake, "p1", project);
    require(!failed.ready && failed.failureCode == kProjectSetupToolchainUnavailable, "...and the check says why: " + failed.failureCode);
    harness.toolchainPrepared = true;

    require(ensure(service, cmake, "p1", project).ready, "Recovered once the toolchain is back");
    std::error_code ignored;
    fs::remove_all(project / "build", ignored);
    assessed = service.assess("p1", project);
    require(!assessed.value().fresh && assessed.value().reason == "build_tree_missing",
        "A deleted build tree (the Run's target discovery needs it) is not ready: " + assessed.value().reason);
    require(ensure(service, cmake, "p1", project).ready && fs::exists(project / "build" / "local" / "CMakeCache.txt"),
        "...and the check recreates it");

    // The same project registered at another folder (a moved or copied workspace) does not inherit the proof.
    const fs::path moved = base.path / "moved";
    makeProject(moved);
    fs::create_directories(moved / "build" / "local");
    writeFile(moved / "build" / "local" / "CMakeCache.txt", "x");
    assessed = service.assess("p1", moved);
    require(!assessed.value().fresh && assessed.value().reason == "project_root_changed", "A different root: " + assessed.value().reason);
}

void testAFileEditedWhileTheCheckRanIsNeverRecordedAsReady()
{
    TestDirectory base("race");
    const fs::path project = base.path / "p";
    makeProject(project);
    Harness harness;
    FakeCmake cmake;
    cmake.onConfigure = [&] { writeFile(project / "CMakeLists.txt", "cmake_minimum_required(VERSION 3.20)\nproject(edited CXX)\n"); };
    auto service = harness.service();
    const auto result = ensure(service, cmake, "p1", project);
    require(!result.ready && result.failureCode == "project_setup.changed_during_check", "Changed while checking: " + result.failureCode);
    const auto record = harness.repository.find("p1").value();
    require(record.has_value() && record->state == ProjectReadinessState::Failed && record->inputsFingerprint.empty(),
        "No Ready record exists for inputs the configure did not see");
}

void testACancelledCheckRecordsNothing()
{
    TestDirectory base("cancel");
    const fs::path project = base.path / "p";
    makeProject(project);
    Harness harness;
    auto service = harness.service();

    FakeCmake early;
    std::stop_source stopped;
    stopped.request_stop();
    auto result = ensure(service, early, "p1", project, false, stopped.get_token());
    require(!result.ready && result.failureCode == kProjectSetupCancelled, "Cancelled before it started");
    require(!harness.repository.find("p1").value().has_value(), "...recorded nothing");

    FakeCmake midway;
    midway.configureCancelled = true;
    result = ensure(service, midway, "p1", project);
    require(!result.ready && result.failureCode == kProjectSetupCancelled, "Cancelled during the configure");
    require(!harness.repository.find("p1").value().has_value(), "...recorded nothing");

    // A cancelled re-check does not turn a good record into a bad one either.
    FakeCmake good;
    require(ensure(service, good, "p1", project).ready, "setup");
    FakeCmake cancelledAgain;
    cancelledAgain.configureCancelled = true;
    result = ensure(service, cancelledAgain, "p1", project, true);
    require(!result.ready && result.failureCode == kProjectSetupCancelled, "A forced re-check can be cancelled");
    require(harness.repository.find("p1").value()->state == ProjectReadinessState::Ready, "...and the proof it did not touch stays");
}

void testAFailureIsRecordedWithItsReasonAndRecoveryWorks()
{
    TestDirectory base("fail");
    const fs::path project = base.path / "p";
    makeProject(project);
    Harness harness;
    FakeCmake cmake;
    cmake.configureExit = 1;
    cmake.configureOutput = "LINK : fatal error LNK1104: cannot open file 'kernel32.lib'\n";
    auto service = harness.service();
    const auto failed = ensure(service, cmake, "p1", project);
    require(!failed.ready && failed.failureCode == kProjectSetupConfigureFailed, "The configure failure is reported");
    const auto record = harness.repository.find("p1").value();
    require(record.has_value() && record->state == ProjectReadinessState::Failed && record->failureCode == kProjectSetupConfigureFailed
            && record->failureMessage.find("kernel32.lib") != std::string::npos,
        "...and recorded with the reason");
    require(record->configurePreset == "local" && record->buildPreset == "local-build", "The automatic choice is kept");
    require(!record->lastSuccessAt.has_value(), "A failure is not a success time");

    // A failed record is never reused: every Run tries again (cheaply) until it passes.
    const auto before = cmake.configureCalls();
    cmake.configureExit = 0;
    require(ensure(service, cmake, "p1", project).ready && cmake.configureCalls() == before + 1, "Fixed -> proven");
    require(harness.repository.find("p1").value()->state == ProjectReadinessState::Ready, "...and recorded ready");
}

void testSeveralPairsWaitForTheUsersChoiceAndForceRecheckRerunsAFreshSetup()
{
    TestDirectory base("pairs");
    const fs::path project = base.path / "p";
    makeProject(project);
    writeFile(project / "CMakePresets.json", R"json({
      "version": 3,
      "configurePresets": [
        {"name": "debug", "generator": "Ninja", "binaryDir": "${sourceDir}/build/debug"},
        {"name": "release", "generator": "Ninja", "binaryDir": "${sourceDir}/build/release"}],
      "buildPresets": [
        {"name": "debug-build", "configurePreset": "debug"},
        {"name": "release-build", "configurePreset": "release"}]})json");
    Harness harness;
    FakeCmake cmake;
    cmake.configureListing = "Available configure presets:\n\n  \"debug\"\n  \"release\"\n";
    cmake.buildListing = "Available build presets:\n\n  \"debug-build\"\n  \"release-build\"\n";
    auto service = harness.service();

    auto result = ensure(service, cmake, "p1", project);
    require(!result.ready && result.failureCode == kProjectSetupSelectionRequired && result.candidates.size() == 2,
        "Two pairs wait for the user");
    require(harness.repository.find("p1").value()->state == ProjectReadinessState::Unverified
            && cmake.configureCalls() == 0,
        "...recorded as unverified, nothing configured");

    require(!service.selectPair("p1", project, {"", ""}).success(), "An incomplete choice is refused");
    require(service.selectPair("p1", project, {"release", "release-build"}).success(), "The user chooses");
    cmake.binaryDir = "build/release";
    result = ensure(service, cmake, "p1", project);
    require(result.ready && result.selection.configurePreset == "release", "The choice is used: " + result.failureMessage);

    const auto configures = cmake.configureCalls();
    require(ensure(service, cmake, "p1", project).ready && cmake.configureCalls() == configures, "Fresh -> reused");
    require(ensure(service, cmake, "p1", project, true).ready && cmake.configureCalls() == configures + 1,
        "The explicit 'check again' proves it again even when it is fresh");

    // Choosing another pair never inherits the old proof.
    require(service.selectPair("p1", project, {"debug", "debug-build"}).success(), "The user changes the choice");
    const auto after = harness.repository.find("p1").value();
    require(after->state == ProjectReadinessState::Unverified && after->inputsFingerprint.empty() && after->targets.empty(),
        "A different pair starts unverified");
}

// ---- SQLite ----

std::string rawSql(const fs::path& databasePath, const char* sql)
{
    sqlite3* handle = nullptr;
    const std::u8string path = databasePath.generic_u8string();
    const std::string utf8Path(reinterpret_cast<const char*>(path.data()), path.size());
    require(sqlite3_open_v2(utf8Path.c_str(), &handle, SQLITE_OPEN_READWRITE, nullptr) == SQLITE_OK, "could not open the database");
    char* rawMessage = nullptr;
    const int result = sqlite3_exec(handle, sql, nullptr, nullptr, &rawMessage);
    std::string message = result == SQLITE_OK ? std::string {}
        : (rawMessage != nullptr ? std::string(rawMessage) : std::string("unknown SQLite error"));
    sqlite3_free(rawMessage);
    sqlite3_close_v2(handle);
    return message;
}

ProjectReadinessRecord sampleRecord(const ProjectId& id)
{
    ProjectReadinessRecord record;
    record.projectId = id;
    record.projectRoot = "C:/work/proj";
    record.configurePreset = "local";
    record.buildPreset = "local-build";
    record.binaryDir = "build/local";
    record.state = ProjectReadinessState::Ready;
    record.inputsFingerprint = std::string(64, 'a');
    record.toolchainIdentity = "msvc=14.51;sdk=10.0.26100.0;vs=aaaa";
    record.targets = {"all", "app", "app_qmllint"};
    record.targetsComplete = true;
    record.checkedAt = std::chrono::system_clock::now();
    record.lastSuccessAt = record.checkedAt;
    return record;
}

Project sampleProject(const ProjectId& id, const fs::path& root)
{
    Project project;
    project.id = id;
    project.name = id;
    project.rootPath = root;
    project.createdAt = std::chrono::system_clock::now();
    project.updatedAt = project.createdAt;
    return project;
}

void testSqliteRecordSurvivesRestartMigratesAndRefusesAnUnprovenReady()
{
    TestDirectory base("db");
    const fs::path databasePath = base.path / "state" / "o.sqlite3";
    {
        const auto opened = ai::infrastructure::SqliteDatabase::open(databasePath);
        require(opened.success(), "Could not open the database");
        ai::infrastructure::SqliteProjectRepository projects(opened.value());
        require(projects.insert(sampleProject("p1", base.path / "p1")).success(), "project row");
        ai::infrastructure::SqliteProjectReadinessRepository repository(opened.value());
        require(repository.find("p1").success() && !repository.find("p1").value().has_value(), "A project starts with no record");
        auto record = sampleRecord("p1");
        record.projectRoot = detail::genericUtf8(base.path / fs::path(u8"\u043f\u0430\u043f\u043a\u0430-\u6e2c\u8a66"));
        require(repository.upsert(record).success(), "The record is stored");
    }
    {
        const auto reopened = ai::infrastructure::SqliteDatabase::open(databasePath);
        require(reopened.success(), "Could not reopen the database");
        ai::infrastructure::SqliteProjectReadinessRepository repository(reopened.value());
        const auto loaded = repository.find("p1");
        require(loaded.success() && loaded.value().has_value(), "The record survives a restart");
        const auto& record = *loaded.value();
        require(record.state == ProjectReadinessState::Ready && record.configurePreset == "local" && record.binaryDir == "build/local"
                && record.targets.size() == 3 && record.targetsComplete && record.lastSuccessAt.has_value()
                && record.toolchainIdentity == "msvc=14.51;sdk=10.0.26100.0;vs=aaaa",
            "...field by field");
        require(record.projectRoot == detail::genericUtf8(base.path / fs::path(u8"\u043f\u0430\u043f\u043a\u0430-\u6e2c\u8a66")),
            "...including a non-ASCII root");

        auto failed = sampleRecord("p1");
        failed.state = ProjectReadinessState::Failed;
        failed.lastSuccessAt.reset();
        failed.failureCode = "project_setup.configure_failed";
        failed.failureMessage = "LNK1104";
        require(repository.upsert(failed).success(), "A second upsert replaces the one record");
        const auto replaced = repository.find("p1").value();
        require(replaced->state == ProjectReadinessState::Failed && !replaced->lastSuccessAt.has_value()
                && replaced->failureCode == "project_setup.configure_failed",
            "...there is exactly one record per project");

        auto unproven = sampleRecord("p1");
        unproven.inputsFingerprint.clear();
        require(!repository.upsert(unproven).success(), "The schema refuses a Ready record without its fingerprint");
        auto ghost = sampleRecord("no-such-project");
        require(!repository.upsert(ghost).success(), "A record needs an existing project");
    }

    // Step 70 is additive and repeatable: a v69 database gets the (empty) table; a rewound v70 keeps its record.
    require(rawSql(databasePath, "DROP TABLE project_verification_readiness; PRAGMA user_version = 69;").empty(), "rewind to v69");
    {
        const auto migrated = ai::infrastructure::SqliteDatabase::open(databasePath);
        require(migrated.success(), "A v69 database did not migrate to v70: " + (migrated.success() ? std::string() : migrated.error()->message));
        ai::infrastructure::SqliteProjectRepository projects(migrated.value());
        require(projects.findById("p1").success(), "Existing rows are untouched");
        ai::infrastructure::SqliteProjectReadinessRepository repository(migrated.value());
        require(!repository.find("p1").value().has_value(), "Existing projects start unverified (no rows)");
        require(repository.upsert(sampleRecord("p1")).success(), "...and accept a record");
    }
    require(rawSql(databasePath, "PRAGMA user_version = 69;").empty(), "rewind the version only");
    {
        const auto again = ai::infrastructure::SqliteDatabase::open(databasePath);
        require(again.success(), "Step 70 must be repeatable over its own table");
        ai::infrastructure::SqliteProjectReadinessRepository repository(again.value());
        require(repository.find("p1").value().has_value(), "Step 70 keeps an existing record");
    }
    require(!rawSql(databasePath, "UPDATE project_verification_readiness SET state = 'maybe';").empty(), "An unknown state is refused");
}

} // namespace

int main()
{
    try {
        testTheFingerprintTracksConfigurationInputsAndNothingElse();
        testASuccessfulSetupIsRecordedSurvivesARestartAndIsNotRepeated();
        testSourceEditsKeepTheStateButConfigurationEditsInvalidateIt();
        testAChangedToolchainRootOrBuildTreeNeedsANewProof();
        testAFileEditedWhileTheCheckRanIsNeverRecordedAsReady();
        testACancelledCheckRecordsNothing();
        testAFailureIsRecordedWithItsReasonAndRecoveryWorks();
        testSeveralPairsWaitForTheUsersChoiceAndForceRecheckRerunsAFreshSetup();
        testSqliteRecordSurvivesRestartMigratesAndRefusesAnUnprovenReady();
    } catch (const std::exception& exception) {
        std::cerr << "ProjectReadiness tests failed: " << exception.what() << std::endl;
        return 1;
    }
    std::cout << "ProjectReadiness tests passed" << std::endl;
    return 0;
}
