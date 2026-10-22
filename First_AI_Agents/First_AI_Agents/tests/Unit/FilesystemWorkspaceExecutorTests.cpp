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

#include "ai/application/FilesystemWorkspaceToolExecutor.hpp"
#include "ai/application/WorkspaceReadPolicy.hpp"

#include <nlohmann/json.hpp>

#include <cassert>
#include <cstdio>
#include <filesystem>
#include <fstream>
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

    return 0;
}
