// FilesystemWorkspaceToolExecutor — unit and adversarial tests.
//
// Coverage (1.3 — per-tool unit tests):
//   workspace_stat_path     — exists, not-found, directory, symlink info
//   workspace_hash_file     — known content, large-file rejection
//   workspace_read_file     — UTF-8, truncation, binary, invalid UTF-8
//   workspace_list_directory — sorted, truncated, non-recursive
//   workspace_find_files    — glob match, recursive, sorted, truncated
//   workspace_search_text   — literal search, snippet, file skip, truncated
//
// Coverage (1.4 — adversarial):
//   traversal / absolute / UNC / ADS → workspace_path.unsafe / denied
//   sensitive filenames / extensions  → workspace_path.denied
//   binary content                    → workspace_read.binary_file
//   invalid UTF-8                     → workspace_read.not_utf8
//   oversized file                    → truncated flag set
//   oversized directory               → truncated flag set
//   too many search results           → truncated flag set
//   secret in content                 → workspace_read.secret_detected / skipped
//   unknown tool                      → tool_call.unknown_tool
//   malformed JSON arguments          → workspace.invalid_arguments
//   unknown JSON key                  → workspace.invalid_arguments
//   wrong JSON value type             → workspace.invalid_arguments
//   no-write / no-rename guarantee    — only IWorkspaceToolExecutor::execute() used
//   no absolute paths in results      — verified on every result

#include "ai/application/workspace/FilesystemWorkspaceToolExecutor.hpp"
#include "ai/application/workspace/WorkspaceReadPolicy.hpp"

#include <nlohmann/json.hpp>

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <string>
#include <vector>

namespace {

namespace app = ai::application;
namespace fs  = std::filesystem;
using json    = nlohmann::json;

// ---------------------------------------------------------------------------
// Test infrastructure
// ---------------------------------------------------------------------------

fs::path makeTempWorkspace()
{
    std::error_code ec;
    const fs::path base = fs::temp_directory_path() / "fwte_test";
    fs::create_directories(base, ec);
    return fs::weakly_canonical(base);
}

void writeFile(const fs::path& path, const std::string& content)
{
    fs::create_directories(path.parent_path());
    std::ofstream ofs(path, std::ios::binary);
    ofs << content;
}

app::AgentToolCall makeCall(const std::string& tool, const std::string& argsJson,
                            const std::string& callId = "test-call-1")
{
    app::AgentToolCall c;
    c.callId       = callId;
    c.toolName     = tool;
    c.argumentsJson = argsJson;
    return c;
}

// Execute and assert the outer Result is always ok (no executor infrastructure failure).
app::AgentToolResult exec(const fs::path& root, const std::string& tool,
                           const std::string& argsJson)
{
    app::FilesystemWorkspaceToolExecutor ex;
    const auto policy = app::WorkspaceReadPolicy::secureDefaults(root);
    auto r = ex.execute(makeCall(tool, argsJson), policy);
    assert(r.success()); // outer Result must always succeed
    return r.value();
}

// Same but with a custom policy.
app::AgentToolResult execP(const app::WorkspaceReadPolicy& policy,
                            const std::string& tool, const std::string& argsJson)
{
    app::FilesystemWorkspaceToolExecutor ex;
    auto r = ex.execute(makeCall(tool, argsJson), policy);
    assert(r.success());
    return r.value();
}

// Assert result has no absolute paths.
void assertNoAbsolutePaths(const app::AgentToolResult& r)
{
    if (!r.success) return; // errors don't embed paths
    // contentJson must not contain Windows or Unix absolute path markers.
    assert(r.contentJson.find("C:\\") == std::string::npos);
    assert(r.contentJson.find("c:\\") == std::string::npos);
    assert(r.contentJson.find("C:/") == std::string::npos);
    assert(r.contentJson.find("c:/") == std::string::npos);
    assert(r.contentJson.find(":\\") == std::string::npos);
    assert(r.contentJson.find(":/") == std::string::npos);
    // Specifically no temp directory prefix.
    const std::string tmp = fs::temp_directory_path().generic_string();
    assert(r.contentJson.find(tmp) == std::string::npos);
}

// ---------------------------------------------------------------------------
// Group 1: workspace_stat_path
// ---------------------------------------------------------------------------

void testStat_existingFile()
{
    const fs::path root = makeTempWorkspace();
    writeFile(root / "hello.txt", "content");
    auto r = exec(root, "workspace_stat_path", R"({"path":"hello.txt"})");
    assert(r.success);
    const auto j = json::parse(r.contentJson);
    assert(j["exists"].get<bool>() == true);
    assert(j["isFile"].get<bool>() == true);
    assert(j["isDirectory"].get<bool>() == false);
    assert(j["relativePath"].get<std::string>() == "hello.txt");
    assertNoAbsolutePaths(r);
    fs::remove_all(root);
}

void testStat_existingDirectory()
{
    const fs::path root = makeTempWorkspace();
    fs::create_directories(root / "subdir");
    auto r = exec(root, "workspace_stat_path", R"({"path":"subdir"})");
    assert(r.success);
    const auto j = json::parse(r.contentJson);
    assert(j["exists"].get<bool>() == true);
    assert(j["isDirectory"].get<bool>() == true);
    assertNoAbsolutePaths(r);
    fs::remove_all(root);
}

void testStat_notFound()
{
    const fs::path root = makeTempWorkspace();
    auto r = exec(root, "workspace_stat_path", R"({"path":"ghost.txt"})");
    assert(r.success);
    const auto j = json::parse(r.contentJson);
    assert(j["exists"].get<bool>() == false);
    assertNoAbsolutePaths(r);
    fs::remove_all(root);
}

void testStat_nestedPath()
{
    const fs::path root = makeTempWorkspace();
    writeFile(root / "src" / "main.cpp", "int main(){}");
    auto r = exec(root, "workspace_stat_path", R"({"path":"src/main.cpp"})");
    assert(r.success);
    const auto j = json::parse(r.contentJson);
    assert(j["exists"].get<bool>() == true);
    assert(j["relativePath"].get<std::string>() == "src/main.cpp");
    assertNoAbsolutePaths(r);
    fs::remove_all(root);
}

void testStat_sizeInResult()
{
    const fs::path root = makeTempWorkspace();
    writeFile(root / "data.bin", std::string(100, 'x'));
    auto r = exec(root, "workspace_stat_path", R"({"path":"data.bin"})");
    assert(r.success);
    const auto j = json::parse(r.contentJson);
    assert(j["sizeBytes"].get<std::int64_t>() == 100);
    assertNoAbsolutePaths(r);
    fs::remove_all(root);
}

// ---------------------------------------------------------------------------
// Group 2: workspace_hash_file
// ---------------------------------------------------------------------------

void testHash_knownContent()
{
    const fs::path root = makeTempWorkspace();
    // SHA-256 of "abc" is ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad
    // (FIPS 180-4 known-answer test vector).
    writeFile(root / "a.txt", "abc");
    auto r = exec(root, "workspace_hash_file", R"({"path":"a.txt"})");
    assert(r.success);
    const auto j = json::parse(r.contentJson);
    assert(j["algorithm"].get<std::string>() == "sha256");
    assert(j["hash"].get<std::string>() ==
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    assert(j["relativePath"].get<std::string>() == "a.txt");
    assertNoAbsolutePaths(r);
    fs::remove_all(root);
}

void testHash_emptyFile()
{
    const fs::path root = makeTempWorkspace();
    // SHA-256 of "" is e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855
    writeFile(root / "empty.txt", "");
    auto r = exec(root, "workspace_hash_file", R"({"path":"empty.txt"})");
    assert(r.success);
    const auto j = json::parse(r.contentJson);
    assert(j["hash"].get<std::string>() ==
        "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    fs::remove_all(root);
}

void testHash_fileTooLarge()
{
    const fs::path root = makeTempWorkspace();
    writeFile(root / "large.bin", std::string(1024, 'x'));
    auto policy = app::WorkspaceReadPolicy::secureDefaults(root);
    policy.maxFileBytes = 512; // file is 1024 bytes, limit is 512
    auto r = execP(policy, "workspace_hash_file", R"({"path":"large.bin"})");
    assert(!r.success);
    assert(r.errorCode == "workspace_hash.file_too_large");
    fs::remove_all(root);
}

void testHash_notAFile()
{
    const fs::path root = makeTempWorkspace();
    fs::create_directories(root / "subdir");
    auto r = exec(root, "workspace_hash_file", R"({"path":"subdir"})");
    assert(!r.success);
    assert(r.errorCode == "workspace_hash.not_a_file");
    fs::remove_all(root);
}

// ---------------------------------------------------------------------------
// Group 3: workspace_read_file
// ---------------------------------------------------------------------------

void testRead_utf8Content()
{
    const fs::path root = makeTempWorkspace();
    writeFile(root / "readme.txt", "Hello, world!");
    auto r = exec(root, "workspace_read_file", R"({"path":"readme.txt"})");
    assert(r.success);
    const auto j = json::parse(r.contentJson);
    assert(j["content"].get<std::string>() == "Hello, world!");
    assert(j["truncated"].get<bool>() == false);
    assert(j["encoding"].get<std::string>() == "utf-8");
    assertNoAbsolutePaths(r);
    fs::remove_all(root);
}

void testRead_truncationAtPolicyLimit()
{
    const fs::path root = makeTempWorkspace();
    writeFile(root / "big.txt", std::string(500, 'a'));
    auto policy = app::WorkspaceReadPolicy::secureDefaults(root);
    policy.maxFileBytes = 100;
    auto r = execP(policy, "workspace_read_file", R"({"path":"big.txt"})");
    assert(r.success);
    assert(r.truncated);
    const auto j = json::parse(r.contentJson);
    assert(j["truncated"].get<bool>() == true);
    // Returned content must be exactly maxFileBytes long.
    assert(j["content"].get<std::string>().size() == 100);
    assertNoAbsolutePaths(r);
    fs::remove_all(root);
}

void testRead_exactlyAtLimit_notTruncated()
{
    const fs::path root = makeTempWorkspace();
    writeFile(root / "exact.txt", std::string(100, 'b'));
    auto policy = app::WorkspaceReadPolicy::secureDefaults(root);
    policy.maxFileBytes = 100;
    auto r = execP(policy, "workspace_read_file", R"({"path":"exact.txt"})");
    assert(r.success);
    assert(!r.truncated);
    fs::remove_all(root);
}

void testRead_notAFile()
{
    const fs::path root = makeTempWorkspace();
    fs::create_directories(root / "dir");
    auto r = exec(root, "workspace_read_file", R"({"path":"dir"})");
    assert(!r.success);
    assert(r.errorCode == "workspace_read.not_a_file");
    fs::remove_all(root);
}

// ---------------------------------------------------------------------------
// Group 4: workspace_list_directory
// ---------------------------------------------------------------------------

void testList_sortedEntries()
{
    const fs::path root = makeTempWorkspace();
    writeFile(root / "src" / "z.cpp", "z");
    writeFile(root / "src" / "a.cpp", "a");
    writeFile(root / "src" / "m.cpp", "m");
    auto r = exec(root, "workspace_list_directory", R"({"path":"src"})");
    assert(r.success);
    const auto j = json::parse(r.contentJson);
    const auto& entries = j["entries"];
    assert(entries.size() == 3);
    assert(entries[0]["name"] == "a.cpp");
    assert(entries[1]["name"] == "m.cpp");
    assert(entries[2]["name"] == "z.cpp");
    assertNoAbsolutePaths(r);
    fs::remove_all(root);
}

// The agent's read-only tools over a workspace whose folder and file names the ANSI code page of this machine cannot hold (Cyrillic on
// an English Windows, Chinese on a Russian one - the names have both): list, read and search must report the names as they are.
void testNonAnsiNamesAreListedReadAndSearchedAsTheyAre()
{
    const fs::path root = makeTempWorkspace() / "nonansi";
    const std::string directory = reinterpret_cast<const char*>(u8"\u043f\u0430\u043f\u043a\u0430-\u6e2c\u8a66");
    const std::string file = reinterpret_cast<const char*>(u8"\u0444\u0430\u0439\u043b-\u6e2c\u8a66.txt");
    const auto utf8Path = [](const std::string& text) {
        return fs::path(std::u8string(reinterpret_cast<const char8_t*>(text.data()), text.size()));
    };
    writeFile(root / utf8Path(directory) / utf8Path(file), "needle in unicode");

    auto listed = exec(root, "workspace_list_directory", json {{"path", directory}}.dump());
    assert(listed.success && "listing a folder with a non-ANSI name must succeed");
    const auto listing = json::parse(listed.contentJson);
    assert(listing["entries"].size() == 1 && listing["entries"][0]["name"] == file);

    auto top = exec(root, "workspace_list_directory", R"({"path":"."})");
    assert(top.success);
    assert(json::parse(top.contentJson)["entries"][0]["name"] == directory);

    auto read = exec(root, "workspace_read_file", json {{"path", directory + "/" + file}}.dump());
    assert(read.success && "reading a file with a non-ANSI name must succeed");
    assert(read.contentJson.find("needle in unicode") != std::string::npos);

    // The glob names part of the file name: matched against the real (UTF-8) name, as a plain name pattern and as a path pattern.
    auto foundByName = exec(root, "workspace_find_files", json {{"path", "."}, {"glob", "*\xE6\xB8\xAC\xE8\xA9\xA6.txt"}}.dump());
    assert(foundByName.success && foundByName.contentJson.find(file) != std::string::npos
        && "a glob with non-ANSI characters must find the file by its name");
    auto foundByPath = exec(root, "workspace_find_files", json {{"path", "."}, {"glob", "*/*\xE6\xB8\xAC\xE8\xA9\xA6.txt"}}.dump());
    assert(foundByPath.success && foundByPath.contentJson.find(file) != std::string::npos
        && "a path glob with non-ANSI characters must find the file by its relative path");

    auto searched = exec(root, "workspace_search_text", json {{"path", "."}, {"query", "needle"}}.dump());
    assert(searched.success && "searching a tree with non-ANSI names must succeed");
    assert(searched.contentJson.find(file) != std::string::npos);
    fs::remove_all(root);
}

void testList_nonRecursive()
{
    const fs::path root = makeTempWorkspace();
    writeFile(root / "top.txt", "");
    writeFile(root / "sub" / "nested.txt", "");
    auto r = exec(root, "workspace_list_directory", R"({"path":"."})");
    assert(r.success);
    const auto j = json::parse(r.contentJson);
    // "sub" directory appears as one entry; "nested.txt" does not appear directly.
    bool foundSub = false, foundNested = false;
    for (const auto& e : j["entries"]) {
        if (e["name"] == "sub") foundSub = true;
        if (e["name"] == "nested.txt") foundNested = true;
    }
    assert(foundSub);
    assert(!foundNested);
    assertNoAbsolutePaths(r);
    fs::remove_all(root);
}

void testList_truncated()
{
    const fs::path root = makeTempWorkspace();
    for (int i = 0; i < 10; ++i)
        writeFile(root / ("f" + std::to_string(i) + ".txt"), "x");
    auto policy = app::WorkspaceReadPolicy::secureDefaults(root);
    policy.maxDirectoryEntries = 3;
    auto r = execP(policy, "workspace_list_directory", R"({"path":"."})");
    assert(r.success);
    assert(r.truncated);
    const auto j = json::parse(r.contentJson);
    assert(j["entries"].size() == 3);
    assert(j["truncated"].get<bool>() == true);
    fs::remove_all(root);
}

void testList_notADirectory()
{
    const fs::path root = makeTempWorkspace();
    writeFile(root / "file.txt", "");
    auto r = exec(root, "workspace_list_directory", R"({"path":"file.txt"})");
    assert(!r.success);
    assert(r.errorCode == "workspace_list.not_a_directory");
    fs::remove_all(root);
}

// ---------------------------------------------------------------------------
// Group 5: workspace_find_files
// ---------------------------------------------------------------------------

void testFind_globMatchesCppFiles()
{
    const fs::path root = makeTempWorkspace();
    writeFile(root / "src" / "main.cpp", "");
    writeFile(root / "src" / "util.cpp", "");
    writeFile(root / "src" / "readme.txt", "");
    auto r = exec(root, "workspace_find_files", R"({"path":"src","glob":"*.cpp"})");
    assert(r.success);
    const auto j = json::parse(r.contentJson);
    const auto& matches = j["matches"];
    assert(matches.size() == 2);
    assertNoAbsolutePaths(r);
    fs::remove_all(root);
}

void testFind_recursive()
{
    const fs::path root = makeTempWorkspace();
    writeFile(root / "a.cpp", "");
    writeFile(root / "sub" / "b.cpp", "");
    writeFile(root / "sub" / "deep" / "c.cpp", "");
    auto r = exec(root, "workspace_find_files", R"({"path":".","glob":"*.cpp"})");
    assert(r.success);
    const auto j = json::parse(r.contentJson);
    assert(j["matches"].size() == 3);
    assertNoAbsolutePaths(r);
    fs::remove_all(root);
}

void testFind_doubleStarPathGlobMatchesTopLevelAndNestedFiles()
{
    const fs::path root = makeTempWorkspace();
    writeFile(root / "src" / "TopLevel.qml", "");
    writeFile(root / "src" / "qml" / "views" / "SettingsView.qml", "");
    writeFile(root / "src" / "qml" / "views" / "notes.txt", "");
    auto r = exec(root, "workspace_find_files", R"({"path":"src","glob":"**/*.qml"})");
    assert(r.success);
    const auto j = json::parse(r.contentJson);
    const auto& matches = j["matches"];
    assert(matches.size() == 2);
    assert((matches[0] == "src/TopLevel.qml" && matches[1] == "src/qml/views/SettingsView.qml")
        || (matches[1] == "src/TopLevel.qml" && matches[0] == "src/qml/views/SettingsView.qml"));
    fs::remove_all(root);
}

void testFind_workspaceRelativePathGlob()
{
    const fs::path root = makeTempWorkspace();
    writeFile(root / "src" / "TopLevel.qml", "");
    writeFile(root / "src" / "qml" / "views" / "SettingsView.qml", "");
    writeFile(root / "other" / "Unrelated.qml", "");
    auto r = exec(root, "workspace_find_files", R"({"path":".","glob":"src/**/*.qml"})");
    assert(r.success);
    const auto j = json::parse(r.contentJson);
    const auto& matches = j["matches"];
    assert(matches.size() == 2);
    assert((matches[0] == "src/TopLevel.qml" && matches[1] == "src/qml/views/SettingsView.qml")
        || (matches[1] == "src/TopLevel.qml" && matches[0] == "src/qml/views/SettingsView.qml"));
    fs::remove_all(root);
}

void testFind_singleStarPathGlobDoesNotCrossDirectories()
{
    const fs::path root = makeTempWorkspace();
    writeFile(root / "src" / "TopLevel.qml", "");
    writeFile(root / "src" / "nested" / "Nested.qml", "");
    auto r = exec(root, "workspace_find_files", R"({"path":".","glob":"src/*.qml"})");
    assert(r.success);
    const auto j = json::parse(r.contentJson);
    assert(j["matches"].size() == 1);
    assert(j["matches"][0] == "src/TopLevel.qml");
    fs::remove_all(root);
}

void testFind_doubleStarDirectorySegmentDoesNotMatchPartialName()
{
    const fs::path root = makeTempWorkspace();
    writeFile(root / "src" / "views" / "Settings.qml", "");
    writeFile(root / "src" / "viewsuffix" / "Settings.qml", "");
    auto r = exec(root, "workspace_find_files",
        R"({"path":"src","glob":"**/views/**/*.qml"})");
    assert(r.success);
    const auto j = json::parse(r.contentJson);
    assert(j["matches"].size() == 1);
    assert(j["matches"][0] == "src/views/Settings.qml");
    fs::remove_all(root);
}

void testFind_sortedResults()
{
    const fs::path root = makeTempWorkspace();
    writeFile(root / "z.cpp", "");
    writeFile(root / "a.cpp", "");
    auto r = exec(root, "workspace_find_files", R"({"path":".","glob":"*.cpp"})");
    assert(r.success);
    const auto j = json::parse(r.contentJson);
    const auto& m = j["matches"];
    assert(m.size() == 2);
    // "a.cpp" should come before "z.cpp".
    const std::string first  = m[0].get<std::string>();
    const std::string second = m[1].get<std::string>();
    assert(first.find("a.cpp") != std::string::npos);
    assert(second.find("z.cpp") != std::string::npos);
    fs::remove_all(root);
}

void testFind_truncated()
{
    const fs::path root = makeTempWorkspace();
    for (int i = 0; i < 10; ++i)
        writeFile(root / ("f" + std::to_string(i) + ".cpp"), "");
    auto policy = app::WorkspaceReadPolicy::secureDefaults(root);
    policy.maxSearchMatches = 4;
    auto r = execP(policy, "workspace_find_files", R"({"path":".","glob":"*.cpp"})");
    assert(r.success);
    assert(r.truncated);
    const auto j = json::parse(r.contentJson);
    assert(j["matches"].size() == 4);
    fs::remove_all(root);
}

void testFind_noMatch()
{
    const fs::path root = makeTempWorkspace();
    writeFile(root / "file.txt", "");
    auto r = exec(root, "workspace_find_files", R"({"path":".","glob":"*.cpp"})");
    assert(r.success);
    const auto j = json::parse(r.contentJson);
    assert(j["matches"].size() == 0);
    fs::remove_all(root);
}

void testFind_emptyGlob_invalidArgs()
{
    const fs::path root = makeTempWorkspace();
    auto r = exec(root, "workspace_find_files", R"({"path":".","glob":""})");
    assert(!r.success);
    // empty glob fails the non-empty required-value check.
    assert(r.errorCode == "workspace.invalid_arguments");
    fs::remove_all(root);
}

// ---------------------------------------------------------------------------
// Group 6: workspace_search_text
// ---------------------------------------------------------------------------

void testSearch_literalMatch()
{
    const fs::path root = makeTempWorkspace();
    writeFile(root / "main.cpp", "// TODO: fix this\nint main() { return 0; }\n");
    auto r = exec(root, "workspace_search_text", R"({"path":".","query":"TODO"})");
    assert(r.success);
    const auto j = json::parse(r.contentJson);
    assert(j["matches"].size() == 1);
    assert(j["matches"][0]["line"].get<int>() == 1);
    assert(j["matches"][0]["file"].get<std::string>().find("main.cpp") != std::string::npos);
    assertNoAbsolutePaths(r);
    fs::remove_all(root);
}

void testSearch_caseSensitive()
{
    const fs::path root = makeTempWorkspace();
    writeFile(root / "f.txt", "hello\nHELLO\n");
    auto r = exec(root, "workspace_search_text", R"({"path":".","query":"hello"})");
    assert(r.success);
    const auto j = json::parse(r.contentJson);
    // Case-sensitive: only lowercase "hello" should match.
    assert(j["matches"].size() == 1);
    fs::remove_all(root);
}

void testSearch_multipleFiles()
{
    const fs::path root = makeTempWorkspace();
    writeFile(root / "a.txt", "needle in a haystack\n");
    writeFile(root / "b.txt", "no match here\n");
    writeFile(root / "c.txt", "another needle\n");
    auto r = exec(root, "workspace_search_text", R"({"path":".","query":"needle"})");
    assert(r.success);
    const auto j = json::parse(r.contentJson);
    assert(j["matches"].size() == 2);
    assertNoAbsolutePaths(r);
    fs::remove_all(root);
}

void testSearch_snippetTruncated()
{
    const fs::path root = makeTempWorkspace();
    // Very long line with needle somewhere inside.
    writeFile(root / "long.txt", std::string(300, 'a') + " needle " + std::string(300, 'b') + "\n");
    auto r = exec(root, "workspace_search_text", R"({"path":".","query":"needle"})");
    assert(r.success);
    const auto j = json::parse(r.contentJson);
    assert(j["matches"].size() == 1);
    // Snippet must be truncated to <=203 chars (200 + "...").
    assert(j["matches"][0]["snippet"].get<std::string>().size() <= 203);
    fs::remove_all(root);
}

void testSearch_truncatedResults()
{
    const fs::path root = makeTempWorkspace();
    // 10 files each with one match.
    for (int i = 0; i < 10; ++i)
        writeFile(root / ("f" + std::to_string(i) + ".txt"), "needle\n");
    auto policy = app::WorkspaceReadPolicy::secureDefaults(root);
    policy.maxSearchMatches = 3;
    auto r = execP(policy, "workspace_search_text", R"({"path":".","query":"needle"})");
    assert(r.success);
    assert(r.truncated);
    const auto j = json::parse(r.contentJson);
    assert(j["matches"].size() == 3);
    fs::remove_all(root);
}

// ---------------------------------------------------------------------------
// Group 7 (1.4): Adversarial — path injection
// ---------------------------------------------------------------------------

void testAdversarial_traversal()
{
    const fs::path root = makeTempWorkspace();
    auto r = exec(root, "workspace_stat_path", R"({"path":"../etc/passwd"})");
    assert(!r.success);
    assert(r.errorCode == "workspace_path.unsafe");
    fs::remove_all(root);
}

void testAdversarial_absolutePathWindows()
{
    const fs::path root = makeTempWorkspace();
    auto r = exec(root, "workspace_read_file", R"({"path":"C:\\windows\\system32\\cmd.exe"})");
    assert(!r.success);
    assert(r.errorCode == "workspace_path.unsafe");
    fs::remove_all(root);
}

void testAdversarial_absolutePathUnix()
{
    const fs::path root = makeTempWorkspace();
    auto r = exec(root, "workspace_read_file", R"({"path":"/etc/passwd"})");
    assert(!r.success);
    assert(r.errorCode == "workspace_path.unsafe");
    fs::remove_all(root);
}

void testAdversarial_unc()
{
    const fs::path root = makeTempWorkspace();
    auto r = exec(root, "workspace_stat_path", R"({"path":"\\\\server\\share\\file"})");
    assert(!r.success);
    assert(r.errorCode == "workspace_path.unsafe");
    fs::remove_all(root);
}

void testAdversarial_ads()
{
    const fs::path root = makeTempWorkspace();
    auto r = exec(root, "workspace_read_file", R"({"path":"file.txt:Zone.Identifier"})");
    assert(!r.success);
    assert(r.errorCode == "workspace_path.unsafe");
    fs::remove_all(root);
}

void testAdversarial_sensitiveFile_git()
{
    const fs::path root = makeTempWorkspace();
    auto r = exec(root, "workspace_read_file", R"({"path":".git/config"})");
    assert(!r.success);
    assert(r.errorCode == "workspace_path.denied");
    fs::remove_all(root);
}

void testAdversarial_sensitiveFile_env()
{
    const fs::path root = makeTempWorkspace();
    auto r = exec(root, "workspace_read_file", R"({"path":".env"})");
    assert(!r.success);
    assert(r.errorCode == "workspace_path.denied");
    fs::remove_all(root);
}

void testAdversarial_sensitiveExtension_pem()
{
    const fs::path root = makeTempWorkspace();
    auto r = exec(root, "workspace_read_file", R"({"path":"certs/server.pem"})");
    assert(!r.success);
    assert(r.errorCode == "workspace_path.denied");
    fs::remove_all(root);
}

void testAdversarial_windowsDeviceName()
{
    const fs::path root = makeTempWorkspace();
    auto r = exec(root, "workspace_read_file", R"({"path":"CON"})");
    assert(!r.success);
#ifdef _WIN32
    assert(r.errorCode == "workspace_path.unsafe");
#else
    // CON is an ordinary, safe filename on POSIX; this fixture did not
    // create it, so the executor must report only that it is not a file.
    assert(r.errorCode == "workspace_read.not_a_file");
#endif
    fs::remove_all(root);
}

void testAdversarial_binaryContent()
{
    const fs::path root = makeTempWorkspace();
    std::string binary = "Hello";
    binary += '\0'; // NUL byte
    binary += "World";
    writeFile(root / "data.bin", binary);
    auto r = exec(root, "workspace_read_file", R"({"path":"data.bin"})");
    assert(!r.success);
    assert(r.errorCode == "workspace_read.binary_file");
    fs::remove_all(root);
}

void testAdversarial_invalidUtf8()
{
    const fs::path root = makeTempWorkspace();
    // Invalid UTF-8: lone continuation byte.
    std::string invalid = "hello ";
    invalid += static_cast<char>(0x80); // invalid UTF-8 byte
    invalid += " world";
    writeFile(root / "bad.txt", invalid);
    auto r = exec(root, "workspace_read_file", R"({"path":"bad.txt"})");
    assert(!r.success);
    assert(r.errorCode == "workspace_read.not_utf8");
    fs::remove_all(root);
}

void testAdversarial_secretInContent_pemKey()
{
    const fs::path root = makeTempWorkspace();
    writeFile(root / "key.txt",
        "some config\n-----BEGIN RSA PRIVATE KEY-----\n"
        "MIIEowIBAAKCAQEAtx7Q1c9mF3sJv6yZk2Nq8rWpXeC5oT4hL1dGqB0aYnV9uJmR\n"
        "wS6fK3xZpQ7cN2hM8vT1aD4rB9eL5gU0iK6jY3sW2nP8oR1qC4tX7bV5zA9mE0hF\n"
        "-----END RSA PRIVATE KEY-----\n");
    auto r = exec(root, "workspace_read_file", R"({"path":"key.txt"})");
    assert(!r.success);
    assert(r.errorCode == "workspace_read.secret_detected");
    fs::remove_all(root);
}

void testAdversarial_secretInContent_githubToken()
{
    const fs::path root = makeTempWorkspace();
    writeFile(root / "notes.txt", "Use token ghp_abcdefghijklmnopqrstuvwxyz12345 for CI");
    auto r = exec(root, "workspace_read_file", R"({"path":"notes.txt"})");
    assert(!r.success);
    assert(r.errorCode == "workspace_read.secret_detected");
    fs::remove_all(root);
}

void testSecretScan_regexPatternDefinitionNotFlagged()
{
    // Source that *implements* secret detection (e.g. ChangeProposalValidator.cpp)
    // legitimately contains these prefixes as regex literals, immediately
    // followed by pattern syntax rather than a real token body. That must
    // read cleanly, not trip the same scanner it documents.
    const fs::path root = makeTempWorkspace();
    writeFile(root / "scanner.cpp",
        "static const std::regex ghToken(R\"(ghp_[a-zA-Z0-9]{36})\");\n"
        "static const std::regex glToken(R\"(glpat-[a-zA-Z0-9_\\-]{20,})\");\n"
        "static const std::regex slackToken(R\"(xoxb-[0-9]+-[0-9]+-[a-zA-Z0-9]+)\");\n"
        "static const std::regex awsKey(R\"(AKIA[A-Z0-9]{16})\");\n");
    auto r = exec(root, "workspace_read_file", R"({"path":"scanner.cpp"})");
    assert(r.success);
    fs::remove_all(root);
}

void testSecretScan_awsDocumentationExampleNotFlagged()
{
    // AKIAIOSFODNN7EXAMPLE is AWS's own well-known non-functional placeholder,
    // used throughout their docs and in this project's own tests.
    const fs::path root = makeTempWorkspace();
    writeFile(root / "notes.txt", "key=AKIAIOSFODNN7EXAMPLE12345678");
    auto r = exec(root, "workspace_read_file", R"({"path":"notes.txt"})");
    assert(r.success);
    fs::remove_all(root);
}

void testSecretScan_docProseMentioningPrefixNotFlagged()
{
    // Documentation naming a token prefix (no real token following it) must
    // not be blocked - only an actual credential-shaped suffix should be.
    const fs::path root = makeTempWorkspace();
    writeFile(root / "CODEX_CONTEXT.md",
        "Secret scanning covers `ghp_`, `gho_`, `github_pat_`, `glpat-`, "
        "`xoxb-`, `xoxp-`, and AWS `AKIA` + 16 uppercase alnum.\n");
    auto r = exec(root, "workspace_read_file", R"({"path":"CODEX_CONTEXT.md"})");
    assert(r.success);
    fs::remove_all(root);
}

void testAdversarial_secretSkippedInSearch()
{
    // A file with a secret should be silently skipped in search, not returned.
    const fs::path root = makeTempWorkspace();
    writeFile(root / "secrets.txt",
        "needle here\n-----BEGIN RSA PRIVATE KEY-----\n"
        "MIIEowIBAAKCAQEAtx7Q1c9mF3sJv6yZk2Nq8rWpXeC5oT4hL1dGqB0aYnV9uJmR\n"
        "-----END RSA PRIVATE KEY-----\n");
    writeFile(root / "safe.txt", "needle here too\n");
    auto r = exec(root, "workspace_search_text", R"({"path":".","query":"needle"})");
    assert(r.success);
    const auto j = json::parse(r.contentJson);
    // Only safe.txt should produce a match; secrets.txt is skipped.
    assert(j["matches"].size() == 1);
    assert(j["matches"][0]["file"].get<std::string>().find("safe.txt") != std::string::npos);
    fs::remove_all(root);
}

void testAdversarial_oversizedFile_truncated()
{
    const fs::path root = makeTempWorkspace();
    writeFile(root / "big.txt", std::string(1000, 'a'));
    auto policy = app::WorkspaceReadPolicy::secureDefaults(root);
    policy.maxFileBytes = 100;
    auto r = execP(policy, "workspace_read_file", R"({"path":"big.txt"})");
    assert(r.success); // succeeds but truncated
    assert(r.truncated);
    fs::remove_all(root);
}

void testAdversarial_oversizedDirectory_truncated()
{
    const fs::path root = makeTempWorkspace();
    for (int i = 0; i < 20; ++i)
        writeFile(root / ("f" + std::to_string(i) + ".txt"), "");
    auto policy = app::WorkspaceReadPolicy::secureDefaults(root);
    policy.maxDirectoryEntries = 5;
    auto r = execP(policy, "workspace_list_directory", R"({"path":"."})");
    assert(r.success);
    assert(r.truncated);
    const auto j = json::parse(r.contentJson);
    assert(j["entries"].size() == 5);
    fs::remove_all(root);
}

void testAdversarial_tooManySearchResults_truncated()
{
    const fs::path root = makeTempWorkspace();
    for (int i = 0; i < 10; ++i)
        writeFile(root / ("f" + std::to_string(i) + ".txt"), "needle\n");
    auto policy = app::WorkspaceReadPolicy::secureDefaults(root);
    policy.maxSearchMatches = 3;
    auto r = execP(policy, "workspace_search_text", R"({"path":".","query":"needle"})");
    assert(r.success);
    assert(r.truncated);
    fs::remove_all(root);
}

// ---------------------------------------------------------------------------
// Group 8 (1.4): Adversarial — argument schema violations
// ---------------------------------------------------------------------------

void testAdversarial_unknownTool()
{
    const fs::path root = makeTempWorkspace();
    app::FilesystemWorkspaceToolExecutor ex;
    const auto policy = app::WorkspaceReadPolicy::secureDefaults(root);
    auto call = makeCall("workspace_delete_file", R"({"path":"file.txt"})");
    auto r = ex.execute(call, policy);
    assert(r.success()); // outer Result ok
    assert(!r.value().success);
    assert(r.value().errorCode == "tool_call.unknown_tool");
    fs::remove_all(root);
}

void testAdversarial_malformedJson()
{
    const fs::path root = makeTempWorkspace();
    auto r = exec(root, "workspace_stat_path", "{not valid json");
    assert(!r.success);
    assert(r.errorCode == "workspace.invalid_arguments");
    fs::remove_all(root);
}

void testAdversarial_unknownJsonKey()
{
    const fs::path root = makeTempWorkspace();
    // "recursive" is not in the schema for workspace_stat_path.
    auto r = exec(root, "workspace_stat_path", R"({"path":"file.txt","recursive":true})");
    assert(!r.success);
    assert(r.errorCode == "workspace.invalid_arguments");
    fs::remove_all(root);
}

void testAdversarial_wrongValueType()
{
    const fs::path root = makeTempWorkspace();
    // "path" must be a string, not a number.
    auto r = exec(root, "workspace_stat_path", R"({"path":42})");
    assert(!r.success);
    assert(r.errorCode == "workspace.invalid_arguments");
    fs::remove_all(root);
}

void testAdversarial_missingRequiredKey()
{
    const fs::path root = makeTempWorkspace();
    // workspace_search_text requires both "path" and "query".
    auto r = exec(root, "workspace_search_text", R"({"path":"."})");
    assert(!r.success);
    assert(r.errorCode == "workspace.invalid_arguments");
    fs::remove_all(root);
}

void testAdversarial_emptyPath()
{
    const fs::path root = makeTempWorkspace();
    auto r = exec(root, "workspace_stat_path", R"({"path":""})");
    assert(!r.success);
    assert(r.errorCode == "workspace.invalid_arguments");
    fs::remove_all(root);
}

void testAdversarial_jsonArray_notObject()
{
    const fs::path root = makeTempWorkspace();
    auto r = exec(root, "workspace_stat_path", R"(["path","file.txt"])");
    assert(!r.success);
    assert(r.errorCode == "workspace.invalid_arguments");
    fs::remove_all(root);
}

// ---------------------------------------------------------------------------
// Group 9 (1.4): No-write guarantee
// ---------------------------------------------------------------------------

void testNoWrite_readFileDoesNotModify()
{
    const fs::path root = makeTempWorkspace();
    const fs::path target = root / "immutable.txt";
    writeFile(target, "original");
    const auto mtimeBefore = fs::last_write_time(target);
    exec(root, "workspace_read_file", R"({"path":"immutable.txt"})");
    const auto mtimeAfter = fs::last_write_time(target);
    assert(mtimeBefore == mtimeAfter);
    fs::remove_all(root);
}

// ---------------------------------------------------------------------------
// Group 10: No absolute paths in any result
// ---------------------------------------------------------------------------

void testResults_neverContainAbsolutePaths()
{
    const fs::path root = makeTempWorkspace();
    writeFile(root / "src" / "main.cpp", "hello world");
    writeFile(root / "src" / "util.cpp", "some code");

    const auto tools = std::vector<std::pair<std::string,std::string>>{
        {"workspace_stat_path",      R"({"path":"src/main.cpp"})"},
        {"workspace_hash_file",      R"({"path":"src/main.cpp"})"},
        {"workspace_read_file",      R"({"path":"src/main.cpp"})"},
        {"workspace_list_directory", R"({"path":"src"})"},
        {"workspace_find_files",     R"({"path":"src","glob":"*.cpp"})"},
        {"workspace_search_text",    R"({"path":"src","query":"hello"})"},
    };

    for (const auto& [tool, args] : tools) {
        auto r = exec(root, tool, args);
        assertNoAbsolutePaths(r);
    }
    fs::remove_all(root);
}

// ---------------------------------------------------------------------------
// Group 11: list / find / search apply the read policy to every entry they come across
//
// A path an agent names directly has always been vetted by the policy. These checks pin that what an agent merely ENUMERATES
// or SEARCHES is vetted the same way: a denied entry is not reported, a denied subtree is not entered, and a denied file's
// content is never matched. They use check() rather than assert() so that they are enforced in a build with NDEBUG as well,
// and so that one run reports every violated expectation instead of stopping at the first.
// ---------------------------------------------------------------------------

int g_enumerationFailures = 0;

void check(bool condition, const std::string& what)
{
    if (condition) return;
    ++g_enumerationFailures;
    std::cerr << "FAIL (enumeration): " << what << "\n";
}

std::string joined(const std::set<std::string>& values)
{
    std::string text = "{";
    for (const auto& value : values) text += (text.size() > 1 ? ", " : "") + value;
    return text + "}";
}

std::set<std::string> listedNames(const app::AgentToolResult& result)
{
    std::set<std::string> names;
    if (!result.success) return names;
    const json parsed = json::parse(result.contentJson); // kept alive: a range-for does not extend a temporary's lifetime before C++23
    for (const auto& entry : parsed["entries"]) names.insert(entry["name"].get<std::string>());
    return names;
}

std::set<std::string> foundPaths(const app::AgentToolResult& result)
{
    std::set<std::string> paths;
    if (!result.success) return paths;
    const json parsed = json::parse(result.contentJson);
    for (const auto& match : parsed["matches"]) paths.insert(match.get<std::string>());
    return paths;
}

std::set<std::string> searchedFiles(const app::AgentToolResult& result)
{
    std::set<std::string> files;
    if (!result.success) return files;
    const json parsed = json::parse(result.contentJson);
    for (const auto& match : parsed["matches"]) files.insert(match["file"].get<std::string>());
    return files;
}

bool resultTruncated(const app::AgentToolResult& result)
{
    return result.success && json::parse(result.contentJson)["truncated"].get<bool>();
}

fs::path freshWorkspace(const char* name)
{
    const fs::path root = makeTempWorkspace() / name;
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root, ec);
    return root;
}

// A tree with allowed files, files denied by name, by the MSVC output sequence and by an explicit deniedPaths entry, a similarly
// named sibling of the denied path, and a secret-looking file that holds plain text (so only the POLICY can hide it).
fs::path buildPolicyTree(const char* name)
{
    const fs::path root = freshWorkspace(name);
    writeFile(root / "README.md", "ENUM_MARKER readme\n");
    writeFile(root / "src" / "main.cpp", "int main() { return 0; } // ENUM_MARKER\n");
    writeFile(root / "generated" / "cache-source" / "keep.txt", "ENUM_MARKER sibling of the denied path\n");
    writeFile(root / "x64" / "keep.txt", "ENUM_MARKER an architecture folder is not output\n");
    writeFile(root / ".git" / "config", "ENUM_MARKER git\n");
    writeFile(root / "build" / "out.txt", "ENUM_MARKER build\n");
    writeFile(root / ".vs" / "state.txt", "ENUM_MARKER visual studio\n");
    writeFile(root / "x64" / "Debug" / "log.txt", "ENUM_MARKER msvc output\n");
    writeFile(root / ".env", "ENUM_MARKER=plain test text\n");
    writeFile(root / "generated" / "cache" / "a.txt", "ENUM_MARKER denied path\n");
    return root;
}

app::WorkspaceReadPolicy treePolicy(const fs::path& root)
{
    auto policy = app::WorkspaceReadPolicy::secureDefaults(root);
    policy.deniedPaths = {"generated/cache"};
    return policy;
}

void testEnumeration_denyRulesApplyToListFindAndSearch()
{
    const fs::path root = buildPolicyTree("enum_policy");
    const auto policy = treePolicy(root);

    const auto top = execP(policy, "workspace_list_directory", R"({"path":"."})");
    check(top.success, "listing the workspace root succeeds");
    const auto names = listedNames(top);
    for (const char* hidden : {".git", "build", ".vs", ".env"}) {
        check(!names.contains(hidden), std::string("list '.' must not report the denied entry ") + hidden + " (got " + joined(names) + ")");
    }
    for (const char* shown : {"README.md", "src", "generated", "x64"}) {
        check(names.contains(shown), std::string("list '.' must still report the allowed entry ") + shown);
    }
    const auto x64 = listedNames(execP(policy, "workspace_list_directory", R"({"path":"x64"})"));
    check(x64 == std::set<std::string> {"keep.txt"},
        "list 'x64' hides the MSVC output sequence x64/Debug but keeps its other content (got " + joined(x64) + ")");
    const auto generated = listedNames(execP(policy, "workspace_list_directory", R"({"path":"generated"})"));
    check(generated == std::set<std::string> {"cache-source"},
        "the denied path generated/cache is hidden while generated/cache-source is not (got " + joined(generated) + ")");

    const std::set<std::string> visibleFiles {
        "README.md", "src/main.cpp", "generated/cache-source/keep.txt", "x64/keep.txt"};

    const auto found = execP(policy, "workspace_find_files", R"({"path":".","glob":"*"})");
    check(found.success, "find from the workspace root succeeds");
    check(foundPaths(found) == visibleFiles,
        "find reports exactly the policy-visible files (got " + joined(foundPaths(found)) + ")");

    const auto searched = execP(policy, "workspace_search_text", R"({"path":".","query":"ENUM_MARKER"})");
    check(searched.success, "search from the workspace root succeeds");
    check(searchedFiles(searched) == visibleFiles,
        "search matches only policy-visible files, including no .env holding plain text (got " + joined(searchedFiles(searched)) + ")");
    check(searched.contentJson.find("plain test text") == std::string::npos, "no content of the denied .env reaches the result");

    // The same policy still refuses a direct read of every one of them.
    for (const char* denied : {".git/config", "build/out.txt", ".vs/state.txt", "x64/Debug/log.txt", ".env", "generated/cache/a.txt"}) {
        const auto direct = execP(policy, "workspace_read_file", json {{"path", denied}}.dump());
        check(!direct.success && direct.errorCode == "workspace_path.denied", std::string("direct read of ") + denied + " stays denied");
    }
    check(execP(policy, "workspace_read_file", R"({"path":"src/main.cpp"})").success, "an allowed file stays readable");
    fs::remove_all(root);
}

void testEnumeration_deniedEntriesNeverDisplaceAllowedOnes()
{
    const fs::path root = freshWorkspace("enum_limits");
    // The denied names sort BEFORE the allowed ones, so a limit applied before the filtering would drop allowed results.
    writeFile(root / ".env", "ENUM_MARKER one\nENUM_MARKER two\nENUM_MARKER three\n");
    writeFile(root / ".git" / "config", "ENUM_MARKER a\nENUM_MARKER b\n");
    writeFile(root / "build" / "out.txt", "ENUM_MARKER c\nENUM_MARKER d\n");
    writeFile(root / "src" / "a.txt", "ENUM_MARKER a\n");
    writeFile(root / "src" / "b.txt", "ENUM_MARKER b\n");
    writeFile(root / "a.txt", "x");
    writeFile(root / "b.txt", "x");
    writeFile(root / "c.txt", "x");

    auto policy = app::WorkspaceReadPolicy::secureDefaults(root);
    policy.maxDirectoryEntries = 3;
    policy.maxSearchMatches = 2;

    // list: exactly three allowed entries (a.txt, b.txt, c.txt, plus src makes four) - the limit counts allowed entries only.
    auto listed = execP(policy, "workspace_list_directory", R"({"path":"."})");
    check(listed.success && listedNames(listed) == std::set<std::string> {"a.txt", "b.txt", "c.txt"} && resultTruncated(listed),
        "list: denied names do not use up the entry limit; the first three allowed entries are returned and truncated is set (got "
            + joined(listedNames(listed)) + ")");
    fs::remove(root / "c.txt");
    fs::remove_all(root / "src");
    listed = execP(policy, "workspace_list_directory", R"({"path":"."})");
    check(listedNames(listed) == std::set<std::string> {"a.txt", "b.txt"} && !resultTruncated(listed),
        "list: when the allowed entries fit the limit, truncated stays false although denied entries exist (got "
            + joined(listedNames(listed)) + ")");
    writeFile(root / "src" / "a.txt", "ENUM_MARKER a\n");
    writeFile(root / "src" / "b.txt", "ENUM_MARKER b\n");
    fs::remove(root / "a.txt");
    fs::remove(root / "b.txt");

    // find and search: the limit is two matches.
    auto found = execP(policy, "workspace_find_files", R"({"path":".","glob":"*"})");
    check(foundPaths(found) == std::set<std::string> {"src/a.txt", "src/b.txt"} && !resultTruncated(found),
        "find: denied files sorted first do not use up the match limit (got " + joined(foundPaths(found)) + ")");
    auto searched = execP(policy, "workspace_search_text", R"({"path":".","query":"ENUM_MARKER"})");
    check(searchedFiles(searched) == std::set<std::string> {"src/a.txt", "src/b.txt"} && !resultTruncated(searched),
        "search: matches in denied files sorted first do not use up the match limit (got " + joined(searchedFiles(searched)) + ")");

    writeFile(root / "src" / "c.txt", "ENUM_MARKER c\n");
    found = execP(policy, "workspace_find_files", R"({"path":".","glob":"*"})");
    check(foundPaths(found) == std::set<std::string> {"src/a.txt", "src/b.txt"} && resultTruncated(found),
        "find: with a third allowed file the limit still applies and truncated is set (got " + joined(foundPaths(found)) + ")");
    searched = execP(policy, "workspace_search_text", R"({"path":".","query":"ENUM_MARKER"})");
    check(searchedFiles(searched) == std::set<std::string> {"src/a.txt", "src/b.txt"} && resultTruncated(searched),
        "search: with a third allowed match the limit still applies and truncated is set (got " + joined(searchedFiles(searched)) + ")");
    fs::remove_all(root);
}

void testEnumeration_allowedRootsAndExactFileScopesStayBounded()
{
    const fs::path root = freshWorkspace("enum_scopes");
    writeFile(root / "src" / "public" / "a.txt", "ENUM_MARKER public\n");
    writeFile(root / "src" / "private" / "b.txt", "ENUM_MARKER private\n");
    writeFile(root / "docs" / "a.md", "ENUM_MARKER authorised\n");
    writeFile(root / "docs" / "b.md", "ENUM_MARKER sibling\n");
    writeFile(root / "other" / "c.txt", "ENUM_MARKER outside the scope\n");

    // A prefix scope with an explicit denied path inside it.
    auto prefix = app::WorkspaceReadPolicy::secureDefaults(root);
    prefix.allowedRoots = {"src"};
    prefix.deniedPaths = {"src/private"};
    check(execP(prefix, "workspace_list_directory", R"({"path":"."})").errorCode == "workspace_path.outside_allowed_roots",
        "a prefix scope still refuses to list above its root");
    check(execP(prefix, "workspace_find_files", R"({"path":".","glob":"*"})").errorCode == "workspace_path.outside_allowed_roots",
        "a prefix scope still refuses to find above its root");
    const auto prefixList = listedNames(execP(prefix, "workspace_list_directory", R"({"path":"src"})"));
    check(prefixList == std::set<std::string> {"public"}, "inside a prefix scope the denied path is hidden (got " + joined(prefixList) + ")");
    const auto prefixFound = foundPaths(execP(prefix, "workspace_find_files", R"({"path":"src","glob":"*"})"));
    check(prefixFound == std::set<std::string> {"src/public/a.txt"}, "find inside a prefix scope skips the denied path (got " + joined(prefixFound) + ")");
    const auto prefixSearched = searchedFiles(execP(prefix, "workspace_search_text", R"({"path":"src","query":"ENUM_MARKER"})"));
    check(prefixSearched == std::set<std::string> {"src/public/a.txt"},
        "search inside a prefix scope skips the denied path (got " + joined(prefixSearched) + ")");

    // An exact file scope names the directory (so it can be addressed) and one file: its sibling is not authorised.
    auto exact = app::WorkspaceReadPolicy::secureDefaults(root);
    exact.allowedRoots = {"docs", "docs/a.md"};
    exact.allowedRootsAreExactPaths = true;
    check(execP(exact, "workspace_list_directory", R"({"path":"."})").errorCode == "workspace_path.outside_allowed_roots",
        "an exact scope still refuses to list the workspace root");
    const auto exactList = listedNames(execP(exact, "workspace_list_directory", R"({"path":"docs"})"));
    check(exactList == std::set<std::string> {"a.md"}, "an exact scope does not reveal an unlisted sibling in list (got " + joined(exactList) + ")");
    const auto exactFound = foundPaths(execP(exact, "workspace_find_files", R"({"path":"docs","glob":"*.md"})"));
    check(exactFound == std::set<std::string> {"docs/a.md"}, "an exact scope does not reveal an unlisted sibling in find (got " + joined(exactFound) + ")");
    const auto exactSearched = searchedFiles(execP(exact, "workspace_search_text", R"({"path":"docs","query":"ENUM_MARKER"})"));
    check(exactSearched == std::set<std::string> {"docs/a.md"}, "an exact scope does not search an unlisted sibling (got " + joined(exactSearched) + ")");
    fs::remove_all(root);
}

enum class LinkKind { None, Symlink, Junction };

// A real directory link inside the workspace: a symbolic link, or where that needs a privilege this machine does not grant, a junction.
LinkKind makeDirectoryLink(const fs::path& link, const fs::path& target)
{
    std::error_code error;
    fs::create_directory_symlink(target, link, error);
    if (!error) return LinkKind::Symlink;
#ifdef _WIN32
    const std::string command = "mklink /J \"" + link.string() + "\" \"" + target.string() + "\" >nul 2>&1";
    if (std::system(("cmd /c " + command).c_str()) == 0 && fs::exists(link)) return LinkKind::Junction;
#endif
    return LinkKind::None;
}

void testEnumeration_linksAreReportedAsMetadataButNeverFollowed()
{
    const fs::path root = freshWorkspace("enum_links");
    const fs::path outside = freshWorkspace("enum_links_outside");
    writeFile(outside / "secret-note.txt", "ENUM_MARKER outside the workspace\n");
    writeFile(root / "real.txt", "ENUM_MARKER inside\n");

    const LinkKind directoryLink = makeDirectoryLink(root / "linked", outside);
    std::error_code error;
    fs::create_symlink(outside / "secret-note.txt", root / "flink.txt", error);
    const bool fileLink = !error;
    if (directoryLink == LinkKind::None && !fileLink) {
        std::cout << "NOTE: this machine allows no symbolic link and no junction here; the link checks were NOT exercised\n";
        fs::remove_all(root);
        fs::remove_all(outside);
        return;
    }

    const auto listed = execP(app::WorkspaceReadPolicy::secureDefaults(root), "workspace_list_directory", R"({"path":"."})");
    check(listed.success && listedNames(listed).contains("real.txt"), "list reports the ordinary file next to the links");
    if (listed.success) {
        const json parsedListing = json::parse(listed.contentJson);
        for (const auto& entry : parsedListing["entries"]) {
            const std::string name = entry["name"].get<std::string>();
            if ((name == "linked" && directoryLink == LinkKind::Symlink) || (name == "flink.txt" && fileLink)) {
                check(entry["isSymlink"].get<bool>(), "list keeps the link metadata of " + name);
            }
        }
    }
    if (fileLink) check(listedNames(listed).contains("flink.txt"), "list still reports a file link as an entry");

    const auto policy = app::WorkspaceReadPolicy::secureDefaults(root);
    const auto found = execP(policy, "workspace_find_files", R"({"path":".","glob":"*"})");
    check(foundPaths(found) == std::set<std::string> {"real.txt"}, "find returns nothing reached through a link (got " + joined(foundPaths(found)) + ")");
    const auto searched = execP(policy, "workspace_search_text", R"({"path":".","query":"ENUM_MARKER"})");
    check(searchedFiles(searched) == std::set<std::string> {"real.txt"},
        "search reads nothing reached through a link (got " + joined(searchedFiles(searched)) + ")");
    check(searched.contentJson.find("outside the workspace") == std::string::npos, "no content from outside the workspace is returned");

    if (directoryLink != LinkKind::None) {
        for (const char* tool : {"workspace_list_directory", "workspace_find_files", "workspace_search_text"}) {
            json arguments {{"path", "linked"}};
            if (std::string(tool) == "workspace_find_files") arguments["glob"] = "*";
            if (std::string(tool) == "workspace_search_text") arguments["query"] = "ENUM_MARKER";
            check(!execP(policy, tool, arguments.dump()).success, std::string(tool) + " refuses to start inside a directory link");
        }
        check(!execP(policy, "workspace_read_file", R"({"path":"linked/secret-note.txt"})").success,
            "reading through a directory link stays refused");
    }

    // Remove the links first: a junction must never be deleted through its target.
    fs::remove(root / "linked", error);
    fs::remove(root / "flink.txt", error);
    fs::remove_all(root);
    fs::remove_all(outside);
}

// An exact file scope names the files an agent may read. A file inside an UNLISTED directory is still reachable by find / search
// started from a listed ancestor: passing through the directory is a technical step, not a right to return it. The right to return
// or read an entry stays exactly the policy's (so the unlisted sibling stays hidden), and a denial anywhere on the way still cuts.
void testEnumeration_exactScopeReachesAllowedDescendantsThroughUnlistedParents()
{
    const fs::path root = freshWorkspace("enum_exact_parents");
    writeFile(root / "docs" / "nested" / "a.md", "ENUM_MARKER authorised\n");
    writeFile(root / "docs" / "nested" / "b.md", "ENUM_MARKER forbidden sibling\n");
    writeFile(root / "docs" / "other" / "c.md", "ENUM_MARKER unrelated directory\n");
    writeFile(root / "docs" / "build" / "x.md", "ENUM_MARKER below a denied directory name\n");

    auto exact = app::WorkspaceReadPolicy::secureDefaults(root);
    exact.allowedRoots = {"docs", "docs/nested/a.md"};
    exact.allowedRootsAreExactPaths = true;
    const std::set<std::string> onlyA {"docs/nested/a.md"};

    check(execP(exact, "workspace_read_file", R"({"path":"docs/nested/a.md"})").success, "the authorised file reads directly");
    check(execP(exact, "workspace_read_file", R"({"path":"docs/nested/b.md"})").errorCode == "workspace_path.outside_allowed_roots",
        "the unlisted sibling does not read directly");
    const auto found = foundPaths(execP(exact, "workspace_find_files", R"({"path":"docs","glob":"*.md"})"));
    check(found == onlyA, "find from the listed ancestor returns the authorised file, and only it (got " + joined(found) + ")");
    const auto searched = searchedFiles(execP(exact, "workspace_search_text", R"({"path":"docs","query":"ENUM_MARKER"})"));
    check(searched == onlyA, "search from the listed ancestor reads the authorised file, and only it (got " + joined(searched) + ")");
    // Passing through a directory does not make it a returnable entry: list still shows only what the scope names.
    const auto listed = listedNames(execP(exact, "workspace_list_directory", R"({"path":"docs"})"));
    check(listed.empty(), "list does not report the unlisted parent directory (got " + joined(listed) + ")");

    // The negative case: denying the parent also denies the file that was authorised before.
    auto deniedParent = exact;
    deniedParent.deniedPaths = {"docs/nested"};
    check(execP(deniedParent, "workspace_read_file", R"({"path":"docs/nested/a.md"})").errorCode == "workspace_path.denied",
        "a denied parent denies the previously authorised file to a direct read");
    const auto foundDenied = foundPaths(execP(deniedParent, "workspace_find_files", R"({"path":"docs","glob":"*.md"})"));
    check(foundDenied.empty(), "find does not pass through a denied parent (got " + joined(foundDenied) + ")");
    const auto searchedDenied = searchedFiles(execP(deniedParent, "workspace_search_text", R"({"path":"docs","query":"ENUM_MARKER"})"));
    check(searchedDenied.empty(), "search does not pass through a denied parent (got " + joined(searchedDenied) + ")");

    // A denied directory NAME on the way cuts as well, even for an explicitly listed file.
    auto deniedName = app::WorkspaceReadPolicy::secureDefaults(root);
    deniedName.allowedRoots = {"docs", "docs/build/x.md"};
    deniedName.allowedRootsAreExactPaths = true;
    check(foundPaths(execP(deniedName, "workspace_find_files", R"({"path":"docs","glob":"*.md"})")).empty(),
        "find does not pass through a directory with a denied name");
    check(searchedFiles(execP(deniedName, "workspace_search_text", R"({"path":"docs","query":"ENUM_MARKER"})")).empty(),
        "search does not pass through a directory with a denied name");
    fs::remove_all(root);
}

// An unexpected exception inside a case is a failure of that case that is reported with its message, not the end of the program.
void runEnumerationCase(const char* name, void (*testCase)())
{
    try {
        testCase();
    } catch (const std::exception& error) {
        check(false, std::string(name) + " threw: " + error.what());
    } catch (...) {
        check(false, std::string(name) + " threw a non-standard exception");
    }
}

} // namespace

int runFilesystemWorkspaceExecutorTests()
{
    // Group 1: workspace_stat_path
    testStat_existingFile();
    testStat_existingDirectory();
    testStat_notFound();
    testStat_nestedPath();
    testStat_sizeInResult();

    // Group 2: workspace_hash_file
    testHash_knownContent();
    testHash_emptyFile();
    testHash_fileTooLarge();
    testHash_notAFile();

    // Group 3: workspace_read_file
    testRead_utf8Content();
    testRead_truncationAtPolicyLimit();
    testRead_exactlyAtLimit_notTruncated();
    testRead_notAFile();

    // Group 4: workspace_list_directory
    testList_sortedEntries();
    testNonAnsiNamesAreListedReadAndSearchedAsTheyAre();
    testList_nonRecursive();
    testList_truncated();
    testList_notADirectory();

    // Group 5: workspace_find_files
    testFind_globMatchesCppFiles();
    testFind_recursive();
    testFind_doubleStarPathGlobMatchesTopLevelAndNestedFiles();
    testFind_workspaceRelativePathGlob();
    testFind_singleStarPathGlobDoesNotCrossDirectories();
    testFind_doubleStarDirectorySegmentDoesNotMatchPartialName();
    testFind_sortedResults();
    testFind_truncated();
    testFind_noMatch();
    testFind_emptyGlob_invalidArgs();

    // Group 6: workspace_search_text
    testSearch_literalMatch();
    testSearch_caseSensitive();
    testSearch_multipleFiles();
    testSearch_snippetTruncated();
    testSearch_truncatedResults();

    // Group 7 (1.4): Path injection adversarial
    testAdversarial_traversal();
    testAdversarial_absolutePathWindows();
    testAdversarial_absolutePathUnix();
    testAdversarial_unc();
    testAdversarial_ads();
    testAdversarial_sensitiveFile_git();
    testAdversarial_sensitiveFile_env();
    testAdversarial_sensitiveExtension_pem();
    testAdversarial_windowsDeviceName();
    testAdversarial_binaryContent();
    testAdversarial_invalidUtf8();
    testAdversarial_secretInContent_pemKey();
    testAdversarial_secretInContent_githubToken();
    testSecretScan_regexPatternDefinitionNotFlagged();
    testSecretScan_awsDocumentationExampleNotFlagged();
    testSecretScan_docProseMentioningPrefixNotFlagged();
    testAdversarial_secretSkippedInSearch();
    testAdversarial_oversizedFile_truncated();
    testAdversarial_oversizedDirectory_truncated();
    testAdversarial_tooManySearchResults_truncated();

    // Group 8 (1.4): Argument schema violations
    testAdversarial_unknownTool();
    testAdversarial_malformedJson();
    testAdversarial_unknownJsonKey();
    testAdversarial_wrongValueType();
    testAdversarial_missingRequiredKey();
    testAdversarial_emptyPath();
    testAdversarial_jsonArray_notObject();

    // Group 9: No-write guarantee
    testNoWrite_readFileDoesNotModify();

    // Group 10: Absolute paths never in results
    testResults_neverContainAbsolutePaths();

    // Group 11: enumeration and search apply the read policy (checked with check(), also under NDEBUG)
    runEnumerationCase("denyRulesApplyToListFindAndSearch", testEnumeration_denyRulesApplyToListFindAndSearch);
    runEnumerationCase("deniedEntriesNeverDisplaceAllowedOnes", testEnumeration_deniedEntriesNeverDisplaceAllowedOnes);
    runEnumerationCase("allowedRootsAndExactFileScopesStayBounded", testEnumeration_allowedRootsAndExactFileScopesStayBounded);
    runEnumerationCase("linksAreReportedAsMetadataButNeverFollowed", testEnumeration_linksAreReportedAsMetadataButNeverFollowed);
    runEnumerationCase("exactScopeReachesAllowedDescendantsThroughUnlistedParents",
        testEnumeration_exactScopeReachesAllowedDescendantsThroughUnlistedParents);

    if (g_enumerationFailures != 0) {
        std::cerr << g_enumerationFailures << " enumeration check(s) failed\n";
        return 1;
    }
    return 0;
}
