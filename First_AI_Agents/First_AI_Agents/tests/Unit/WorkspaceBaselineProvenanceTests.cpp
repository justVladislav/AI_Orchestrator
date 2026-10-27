#include "ai/application/WorkspaceBaselineProvenance.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace {

namespace fs = std::filesystem;
using namespace ai::application;

void require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

struct TempWorkspace {
    fs::path path;

    TempWorkspace()
    {
        static std::atomic<std::uint64_t> sequence {0};
        path = fs::temp_directory_path()
            / ("ai-orchestrator-baseline-test-"
                + std::to_string(std::chrono::steady_clock::now()
                      .time_since_epoch().count())
                + "-" + std::to_string(sequence.fetch_add(1)));
        fs::create_directories(path);
    }

    ~TempWorkspace()
    {
        std::error_code ignored;
        fs::remove_all(path, ignored);
    }
};

void writeFile(const fs::path& path, std::string_view content)
{
    fs::create_directories(path.parent_path());
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream.write(content.data(), static_cast<std::streamsize>(content.size()));
    require(static_cast<bool>(stream), "Could not write baseline fixture");
}

WorkspaceReadPolicy policyFor(const fs::path& root)
{
    return WorkspaceReadPolicy::secureDefaults(fs::absolute(root));
}

void testCaptureIsDeterministicBoundedAndNonLeaking()
{
    TempWorkspace workspace;
    writeFile(workspace.path / "zeta.qml", "Item {}\n");
    writeFile(workspace.path / "src" / "alpha.cpp", "// source payload\n");
    writeFile(workspace.path / ".env", "SECRET_MUST_NOT_APPEAR");
    writeFile(workspace.path / "build" / "generated.cpp", "excluded build output");
    writeFile(workspace.path / ".git" / "HEAD",
        "0123456789abcdef0123456789abcdef01234567\n");
    // Real-Run incident (Run bb56e001-eac0-4b59-8d0b-d037c6bc1031, 2026-09-17):
    // Qt Creator rewrites CMakeLists.txt.user and regenerates a fresh
    // random-hash qtc-cmake-presets-<hash>/ directory on every session, with
    // zero source edits - this made baseline-drift permanently false-positive
    // for any Qt Creator user. Excluded the same way build/.git already are.
    writeFile(workspace.path / ".qtcreator" / "CMakeLists.txt.user", "<qtcreator-session-state/>");

    const auto first = WorkspaceBaselineProvenance::capture(
        policyFor(workspace.path));
    const auto second = WorkspaceBaselineProvenance::capture(
        policyFor(workspace.path));
    require(first.success() && second.success(), "Could not capture baseline");
    require(first.value().fingerprint == second.value().fingerprint
            && first.value().files.size() == 2,
        "Baseline capture was not deterministic or did not honor exclusions");
    require(first.value().files[0].relativePath == "src/alpha.cpp"
            && first.value().files[1].relativePath == "zeta.qml",
        "Baseline paths were not normalized and deterministically ordered");
    require(first.value().repositoryRevisionAvailable
            && first.value().repositoryRevision.has_value(),
        "Git revision availability was not recorded");

    const auto rendered = WorkspaceBaselineProvenance::renderJson(first.value());
    require(rendered.success(), "Could not render baseline manifest");
    require(rendered.value().find(workspace.path.generic_string()) == std::string::npos
            && rendered.value().find("SECRET_MUST_NOT_APPEAR") == std::string::npos
            && rendered.value().find("source payload") == std::string::npos,
        "Baseline manifest leaked an absolute path or file content");
    require(rendered.value() == WorkspaceBaselineProvenance::renderJson(
                second.value()).value(),
        "Repeated baseline serialization was not byte-identical");
    const auto parsed = WorkspaceBaselineProvenance::parseJson(rendered.value());
    require(parsed.success()
            && parsed.value().fingerprint == first.value().fingerprint
            && parsed.value().files.size() == first.value().files.size(),
        "Baseline manifest did not survive a strict restart parse");

    auto corruptedIdentity = rendered.value();
    const auto fingerprintPosition = corruptedIdentity.find(first.value().fingerprint);
    require(fingerprintPosition != std::string::npos,
        "Rendered baseline did not contain its fingerprint");
    corruptedIdentity[fingerprintPosition] =
        corruptedIdentity[fingerprintPosition] == '0' ? '1' : '0';
    const auto corrupted = WorkspaceBaselineProvenance::parseJson(corruptedIdentity);
    require(!corrupted.success()
            && corrupted.error()->code == "workspace_baseline.identity_mismatch",
        "Semantically corrupted baseline manifest did not fail closed");
}

void testVisualStudioCacheIsExcludedWithoutHidingProjectFiles()
{
    TempWorkspace workspace;
    const auto policy = policyFor(workspace.path);
    const std::string oversizedCache(policy.maxFileBytes + 1, 'x');
    writeFile(workspace.path / ".vs" / "project" / "Browse.VC.db", oversizedCache);
    writeFile(workspace.path / "src" / ".vs" / "index.ipch", oversizedCache);
    writeFile(workspace.path / ".vsconfig", "{\"components\": []}\n");
    writeFile(workspace.path / "src" / "main.cpp", "int main() { return 0; }\n");

    for (const auto& path : {fs::path(".vs/project/Browse.VC.db"),
             fs::path("src/.vs/index.ipch")}) {
        const auto denied = WorkspaceReadPolicyValidator::validateRelativePath(path, policy);
        require(!denied.success() && denied.error()->code == "workspace_path.denied",
            "Visual Studio cache remained readable by workspace tools");
    }
    require(WorkspaceReadPolicyValidator::validateRelativePath(".vsconfig", policy).success(),
        "The Visual Studio workload configuration must remain readable");

    const auto first = WorkspaceBaselineProvenance::capture(policy);
    require(first.success() && first.value().files.size() == 2
            && first.value().files[0].relativePath == ".vsconfig"
            && first.value().files[1].relativePath == "src/main.cpp",
        "Oversized Visual Studio caches must be skipped without hiding project inputs");
    require(std::find(first.value().exclusions.begin(), first.value().exclusions.end(),
                "directory-name:.vs") != first.value().exclusions.end(),
        "The snapshot did not record the Visual Studio cache exclusion");

    writeFile(workspace.path / ".vs" / "project" / "Browse.VC.db", "changed IDE cache");
    const auto second = WorkspaceBaselineProvenance::capture(policy);
    require(second.success() && second.value().fingerprint == first.value().fingerprint,
        "Visual Studio housekeeping changed the source baseline fingerprint");
}

void testVisualStudioBuildDirectoriesAreExcludedAtAnyDepth()
{
    TempWorkspace workspace;
    auto policy = policyFor(workspace.path);
    policy.maxFileBytes = 16;
    const std::vector<fs::path> outputs {
        "VfpDbNative/VfpDbNative/x64/Debug/BitOperations.obj",
        "VfpDbNative/x64/Release/native.pdb",
        "Win32/Debug/native.pch", "sub/Win32/Release/native.dll"};
    const std::vector<fs::path> inputs {
        "src/Debug/Diagnostics.cpp", "src/Release/Notes.cpp",
        "src/x64/Intrinsics.hpp", "src/Win32/Platform.hpp",
        "src/x64/Debugging/Helper.cpp", "data/table.json",
        "resources/icon.svg", "cmake/config.cmake", "CMakeLists.txt"};
    for (const auto& path : outputs) {
        writeFile(workspace.path / path, std::string(policy.maxFileBytes + 1, 'x'));
        const auto denied = WorkspaceReadPolicyValidator::validateRelativePath(path, policy);
        require(!denied.success() && denied.error()->code == "workspace_path.denied",
            "MSVC output remained readable by workspace tools");
    }
    for (const auto& path : inputs) {
        writeFile(workspace.path / path, "project input");
        require(WorkspaceReadPolicyValidator::validateRelativePath(path, policy).success(),
            "The MSVC output rule hid an unrelated project input");
    }
#ifdef _WIN32
    require(!WorkspaceReadPolicyValidator::validateRelativePath(
                "nested/X64/DEBUG/native.obj", policy).success(),
        "MSVC output matching must be case-insensitive on Windows");
#endif
    const auto first = WorkspaceBaselineProvenance::capture(policy);
    require(first.success() && first.value().files.size() == inputs.size(),
        "Baseline must skip oversized MSVC output and retain all project inputs");
    require(std::find(first.value().exclusions.begin(), first.value().exclusions.end(),
                "directory-sequence:x64/Debug") != first.value().exclusions.end(),
        "The baseline must record its MSVC output exclusion rule");
    writeFile(workspace.path / outputs.front(), "changed output");
    const auto second = WorkspaceBaselineProvenance::capture(policy);
    require(second.success() && second.value().fingerprint == first.value().fingerprint,
        "Rebuilding MSVC output changed the source baseline fingerprint");

    policy.allowedRoots = {"VfpDbNative/VfpDbNative/x64/Debug"};
    require(!WorkspaceReadPolicyValidator::validateRelativePath(outputs.front(), policy).success(),
        "An allowed root must not override the MSVC output exclusion");
    const auto scoped = WorkspaceBaselineProvenance::capture(policy);
    require(scoped.success() && scoped.value().files.empty(),
        "Baseline traversal starting inside MSVC output bypassed the exclusion");
}

void testFileLimitDiagnosticsNameRelativeFileAndCorrectBudget()
{
    TempWorkspace workspace;
    writeFile(workspace.path / fs::path(u8"src/\u6e2c\u8a66.cpp"), "12345");
    auto policy = policyFor(workspace.path);
    policy.maxFileBytes = 4;
    const auto oversized = WorkspaceBaselineProvenance::capture(policy);
    require(!oversized.success() && oversized.error()->code == "workspace_baseline.file_limit",
        "An oversized source file must still fail closed");
    require(oversized.error()->message
            == "Policy-visible workspace file \"src/\xe6\xb8\xac\xe8\xa9\xa6.cpp\" is 5 bytes; per-file limit is 4 bytes",
        "The file-limit diagnostic must name the UTF-8 relative path, size and limit only");
    policy.maxFileBytes = 5;
    require(WorkspaceBaselineProvenance::capture(policy).success(),
        "A file exactly at the byte limit must remain permitted");

    TempWorkspace aggregate;
    writeFile(aggregate.path / "first" / "a.cpp", "1234");
    writeFile(aggregate.path / "second" / "b.cpp", "5678");
    auto aggregatePolicy = policyFor(aggregate.path);
    aggregatePolicy.allowedRoots = {"first", "second"};
    WorkspaceBaselineLimits limits;
    limits.maxTotalBytes = 6;
    const auto totalOverflow = WorkspaceBaselineProvenance::capture(aggregatePolicy, limits);
    require(!totalOverflow.success()
            && totalOverflow.error()->code == "workspace_baseline.file_limit",
        "The aggregate baseline limit must still fail closed");
    require(totalOverflow.error()->message
            == "Policy-visible workspace file \"second/b.cpp\" is 4 bytes; remaining baseline byte budget is 2 bytes (total baseline limit: 6 bytes)",
        "The aggregate-limit diagnostic must identify the remaining budget, not blame the per-file limit");
}

void testNonGitWorkspaceAndManualDriftClassification()
{
    TempWorkspace workspace;
    writeFile(workspace.path / "qml" / "Main.qml", "Item {}\n");
    const auto baseline = WorkspaceBaselineProvenance::capture(
        policyFor(workspace.path));
    require(baseline.success() && !baseline.value().repositoryRevisionAvailable
            && !baseline.value().repositoryRevision.has_value(),
        "Non-Git workspace pretended to have repository revision data");

    writeFile(workspace.path / "qml" / "Main.qml", "Item { property int x: 1 }\n");
    writeFile(workspace.path / "qml" / "Manual.qml", "Item {}\n");
    const auto current = WorkspaceBaselineProvenance::capture(
        policyFor(workspace.path));
    require(current.success(), "Could not recapture changed workspace");
    const auto difference = WorkspaceBaselineProvenance::compare(
        baseline.value(), current.value());
    require(difference.success()
            && difference.value().classification == "manual_or_external_drift"
            && difference.value().modifiedPaths
                == std::vector<std::string> {"qml/Main.qml"}
            && difference.value().addedPaths
                == std::vector<std::string> {"qml/Manual.qml"},
        "Manual/external workspace drift was not classified deterministically");
}

// MW-P5.6D: the exact property ManifestVerificationLegacyBaselineNormalizationService
// relies on - pruning a snapshot by a prefix must reproduce BIT-FOR-BIT the
// same fingerprint a fresh capture() would produce once that same prefix is
// excluded at the policy level. This is what lets prepare() prove a durable
// (pre-fix) baseline and the live (post-fix) workspace agree everywhere
// except the excluded delta, without ever touching the filesystem again.
void testPruneExcludedPrefixesMatchesLiveCaptureAfterExclusion()
{
    TempWorkspace workspace;
    writeFile(workspace.path / "qml" / "Main.qml", "Item {}\n");
    writeFile(workspace.path / ".qtcreator" / "CMakeLists.txt.user", "<session/>");
    auto policyWithQtCreatorVisible = policyFor(workspace.path);
    policyWithQtCreatorVisible.deniedDirectoryNames.erase(
        std::remove(policyWithQtCreatorVisible.deniedDirectoryNames.begin(),
            policyWithQtCreatorVisible.deniedDirectoryNames.end(), ".qtcreator"),
        policyWithQtCreatorVisible.deniedDirectoryNames.end());
    const auto durable = WorkspaceBaselineProvenance::capture(policyWithQtCreatorVisible);
    require(durable.success() && durable.value().files.size() == 2,
        "Could not capture the legacy baseline with .qtcreator visible");

    // Simulate the IDE rewriting its own housekeeping file - the exact real
    // shape of the bb56e001 drift.
    writeFile(workspace.path / ".qtcreator" / "CMakeLists.txt.user", "<rewritten-session/>");
    const auto current = WorkspaceBaselineProvenance::capture(policyFor(workspace.path));
    require(current.success() && current.value().files.size() == 1,
        "Could not capture the current, policy-excluded baseline");

    const auto pruned = WorkspaceBaselineProvenance::pruneExcludedPrefixes(
        durable.value(), {".qtcreator/"});
    require(pruned.success() && pruned.value().files.size() == 1
            && pruned.value().files.front().relativePath == "qml/Main.qml",
        "Pruning did not remove exactly the excluded entries");
    require(pruned.value().fingerprint == current.value().fingerprint,
        "A pruned legacy baseline must fingerprint identically to a fresh policy-excluded "
        "capture once the only real difference is under the excluded prefix");
}

void testPruneExcludedPrefixesRejectsUnsafePrefixesAndEmptyList()
{
    TempWorkspace workspace;
    writeFile(workspace.path / "qml" / "Main.qml", "Item {}\n");
    const auto snapshot = WorkspaceBaselineProvenance::capture(policyFor(workspace.path));
    require(snapshot.success(), "Could not capture baseline fixture");

    require(!WorkspaceBaselineProvenance::pruneExcludedPrefixes(snapshot.value(), {}).success(),
        "An empty excluded-prefix list must be rejected");
    for (const std::string& unsafe : {std::string("/abs/"), std::string("no-trailing-slash"),
             std::string("../escape/"), std::string("a\\b/"), std::string("c:/x/")}) {
        const auto result = WorkspaceBaselineProvenance::pruneExcludedPrefixes(
            snapshot.value(), {unsafe});
        require(!result.success() && result.error()->code == "workspace_baseline.invalid_prefix",
            "An unsafe or non-directory-shaped prefix must be rejected");
    }
}

void testFileCountAndByteCapsFailClosed()
{
    TempWorkspace workspace;
    writeFile(workspace.path / "one.txt", "1234");
    writeFile(workspace.path / "two.txt", "5678");
    auto policy = policyFor(workspace.path);
    policy.maxFileBytes = 3;
    const auto oversized = WorkspaceBaselineProvenance::capture(policy);
    require(!oversized.success()
            && oversized.error()->code == "workspace_baseline.file_limit",
        "Oversized policy-visible file did not fail closed");

    policy.maxFileBytes = 1024;
    WorkspaceBaselineLimits limits;
    limits.maxFiles = 1;
    const auto tooMany = WorkspaceBaselineProvenance::capture(policy, limits);
    require(!tooMany.success()
            && tooMany.error()->code == "workspace_baseline.workspace_limit",
        "Baseline file-count overflow did not fail closed");
}

void testAllowedRootContainmentAndVisibleLinksFailClosed()
{
    TempWorkspace workspace;
    auto missingRootPolicy = policyFor(workspace.path);
    missingRootPolicy.allowedRoots = {"missing"};
    const auto missing = WorkspaceBaselineProvenance::capture(missingRootPolicy);
    require(!missing.success()
            && missing.error()->code == "workspace_baseline.invalid_allowed_root",
        "Missing configured allowed root did not fail closed");

    TempWorkspace outside;
    writeFile(outside.path / "outside.txt", "outside");
    std::error_code error;
    fs::create_directory_symlink(outside.path, workspace.path / "linked-root", error);
    if (!error) {
        auto linkedRootPolicy = policyFor(workspace.path);
        linkedRootPolicy.allowedRoots = {"linked-root"};
        const auto escaped = WorkspaceBaselineProvenance::capture(linkedRootPolicy);
        require(!escaped.success()
                && escaped.error()->code == "workspace_baseline.invalid_allowed_root",
            "Symlinked allowed root escaped baseline containment");
    }

    error.clear();
    fs::create_symlink(outside.path / "outside.txt",
        workspace.path / "visible-link.txt", error);
    if (!error) {
        const auto linkedFile = WorkspaceBaselineProvenance::capture(
            policyFor(workspace.path));
        require(!linkedFile.success()
                && linkedFile.error()->code == "workspace_baseline.reparse_point",
            "Policy-visible file link did not fail closed");
    }
}

void testConfiguredCMakePreflightRunsBeforeProviderBoundary()
{
    TempWorkspace workspace;
    auto policy = policyFor(workspace.path);
    const auto absent = WorkspaceBaselineProvenance::runConfiguredCMakePreflight(policy);
    require(absent.success() && !absent.value().configured,
        "Absent preflight configuration was not a successful no-op");

    writeFile(workspace.path / "CMakeLists.txt",
        "qt_add_executable(appUI main.cpp)\ntarget_link_libraries(appUI PRIVATE Dashboard_Content)\n");
    writeFile(workspace.path / "qml" / "CMakeLists.txt",
        "qt_add_qml_module(Dashboard_Content URI Dashboard)\n");
    writeFile(workspace.path / ".ai-orchestrator" / "preflight.json",
        R"({"schemaVersion":"workspace-preflight.v1","cmakeTargetConsistency":[{"id":"dashboard-module","definitionFile":"CMakeLists.txt","registrationFile":"qml/CMakeLists.txt","target":"Dashboard_Content"}]})");
    const auto passed = WorkspaceBaselineProvenance::runConfiguredCMakePreflight(policy);
    require(passed.success() && passed.value().configured
            && passed.value().passedCheckIds
                == std::vector<std::string> {"dashboard-module"},
        "Configured CMake consistency preflight did not pass matching files");

    writeFile(workspace.path / "qml" / "CMakeLists.txt",
        "qt_add_qml_module(Other_Content URI Dashboard)\n");
    const auto mismatch = WorkspaceBaselineProvenance::runConfiguredCMakePreflight(policy);
    require(!mismatch.success()
            && mismatch.error()->code == "workspace_preflight.cmake_target_mismatch",
        "Configured CMake target mismatch did not fail with the typed preflight code");
}

// AG-15: the Planner-facing discovery catalog must be deterministic (same
// input snapshot -> byte-identical output), never leak excluded/secret
// content (capture() already excludes it - this only proves the renderer
// doesn't somehow reintroduce it), and genuinely bound its own size rather
// than emit a partial/truncated line when the byte budget is exceeded.
void testRenderDiscoveryCatalogIsBoundedDeterministicAndSecretFree()
{
    TempWorkspace workspace;
    writeFile(workspace.path / "alpha.cpp", "// alpha\n");
    writeFile(workspace.path / "beta.qml", "Item {}\n");
    writeFile(workspace.path / ".env", "SECRET_MUST_NOT_APPEAR");
    auto policy = policyFor(workspace.path);
    const auto snapshot = WorkspaceBaselineProvenance::capture(policy);
    require(snapshot.success(), "Baseline capture failed for discovery catalog test");

    const auto rendered = WorkspaceBaselineProvenance::renderDiscoveryCatalog(snapshot.value());
    require(rendered.find("alpha.cpp") != std::string::npos,
        "Discovery catalog did not name a real workspace file");
    require(rendered.find("SECRET_MUST_NOT_APPEAR") == std::string::npos,
        "Discovery catalog leaked excluded secret file content");
    require(rendered.find(".env") == std::string::npos,
        "Discovery catalog listed a globally excluded path");

    const auto renderedAgain = WorkspaceBaselineProvenance::renderDiscoveryCatalog(snapshot.value());
    require(rendered == renderedAgain,
        "Discovery catalog rendering was not deterministic for the same snapshot");

    // A byte budget too small even for the header must never emit a
    // truncated/partial entry - the bounded result may be empty, but every
    // byte in it must belong to a complete line.
    const auto tiny = WorkspaceBaselineProvenance::renderDiscoveryCatalog(
        snapshot.value(), /*maxEntries=*/2000, /*maxBytes=*/8);
    require(tiny.size() <= 8, "Discovery catalog exceeded its own tiny byte budget");

    // A maxEntries of zero must produce no file lines at all, regardless of
    // how generous the byte budget is.
    const auto zeroEntries = WorkspaceBaselineProvenance::renderDiscoveryCatalog(
        snapshot.value(), /*maxEntries=*/0, /*maxBytes=*/64ULL * 1024);
    require(zeroEntries.find("alpha.cpp") == std::string::npos,
        "A maxEntries=0 discovery catalog still listed a file");
    require(zeroEntries.find("# Catalog truncated: included 0 of 2 files")
            != std::string::npos,
        "A bounded discovery catalog silently hid omitted files");
}

} // namespace

int main()
{
    try {
        testCaptureIsDeterministicBoundedAndNonLeaking();
        testVisualStudioCacheIsExcludedWithoutHidingProjectFiles();
        testVisualStudioBuildDirectoriesAreExcludedAtAnyDepth();
        testFileLimitDiagnosticsNameRelativeFileAndCorrectBudget();
        testNonGitWorkspaceAndManualDriftClassification();
        testPruneExcludedPrefixesMatchesLiveCaptureAfterExclusion();
        testPruneExcludedPrefixesRejectsUnsafePrefixesAndEmptyList();
        testFileCountAndByteCapsFailClosed();
        testAllowedRootContainmentAndVisibleLinksFailClosed();
        testConfiguredCMakePreflightRunsBeforeProviderBoundary();
        testRenderDiscoveryCatalogIsBoundedDeterministicAndSecretFree();
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "Workspace baseline tests failed: " << exception.what() << '\n';
        return 1;
    }
}
