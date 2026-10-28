// WorkspacePathGuard contract tests.
//
// Coverage:
//   Group 1  — Basic success paths (require a real temp workspace)
//   Group 2  — Lexical denials: traversal, absolute, UNC, ADS, Windows
//               device names, NUL byte, deny lists
//   Group 3  — Trusted-policy validation errors
//   Group 4  — Custom policy lists and allowed-roots enforcement
//   Group 5  — Error messages contain no absolute paths or model input

#include "ai/application/workspace/WorkspacePathGuard.hpp"
#include "ai/application/workspace/WorkspaceReadPolicy.hpp"

#include "ai/application/workspace/FilesystemPathUtils.hpp"

#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

namespace app = ai::application;
namespace fs  = std::filesystem;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// Creates a unique temporary workspace directory and returns its canonical path.
// Tests that use this helper must call fs::remove_all(root) when done.
fs::path makeTempWorkspace()
{
    std::error_code ec;
    const fs::path base = fs::temp_directory_path() / "ai_wpg_test";
    fs::create_directories(base, ec);
    return fs::weakly_canonical(base);
}

app::WorkspaceReadPolicy policyFor(const fs::path& root)
{
    return app::WorkspaceReadPolicy::secureDefaults(root);
}

// Writes a small file at path, creating parent directories as needed.
void writeFile(const fs::path& path, const std::string& content = "test")
{
    fs::create_directories(path.parent_path());
    std::ofstream ofs(path);
    ofs << content;
}

// Fake absolute workspace root that never touches the real filesystem for
// purely lexical tests. The lexical validator runs before any stat() call,
// so the root directory need not exist.
app::WorkspaceReadPolicy lexicalPolicy()
{
#ifdef _WIN32
    return policyFor(fs::path("C:\\projects\\workspace"));
#else
    return policyFor(fs::path("/projects/workspace"));
#endif
}

// ---------------------------------------------------------------------------
// Group 1: Basic success — requires a real temp workspace
// ---------------------------------------------------------------------------

void testCleanRelativePath_returnsAbsoluteCanonical()
{
    const fs::path root = makeTempWorkspace();
    writeFile(root / "hello.txt");
    const auto policy = policyFor(root);

    auto r = app::WorkspacePathGuard::validate("hello.txt", policy);
    assert(r.success());
    assert(r.value().is_absolute());
    // Returned path must sit inside the workspace root.
    std::string resultStr = r.value().generic_string();
    std::string rootStr   = root.generic_string();
#ifdef _WIN32
    // Case-insensitive comparison on Windows.
    for (auto& c : resultStr) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    for (auto& c : rootStr)   c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
#endif
    assert(resultStr.starts_with(rootStr));
    fs::remove_all(root);
}

void testNestedPath_returnsAbsoluteCanonical()
{
    const fs::path root = makeTempWorkspace();
    writeFile(root / "src" / "main.cpp");
    const auto policy = policyFor(root);

    auto r = app::WorkspacePathGuard::validate("src/main.cpp", policy);
    assert(r.success());
    assert(r.value().is_absolute());
    fs::remove_all(root);
}

void testNonExistentFile_guardPassesExistenceCheckToExecutor()
{
    // validate() does NOT check file existence — the executor does.
    // A path that is lexically and canonically safe must pass even when
    // the file does not exist yet (validate-before-open pattern).
    const fs::path root = makeTempWorkspace();
    // root exists; "ghost.txt" inside it does not.
    const auto policy = policyFor(root);

    auto r = app::WorkspacePathGuard::validate("ghost.txt", policy);
    assert(r.success());
    assert(r.value().is_absolute());
    fs::remove_all(root);
}

void testIdempotent_samePathTwice()
{
    const fs::path root = makeTempWorkspace();
    writeFile(root / "data.json");
    const auto policy = policyFor(root);

    auto r1 = app::WorkspacePathGuard::validate("data.json", policy);
    auto r2 = app::WorkspacePathGuard::validate("data.json", policy);
    assert(r1.success() && r2.success());
    assert(r1.value() == r2.value());
    fs::remove_all(root);
}

// Folder and file names that no single Windows ANSI code page can hold (CJK, plus Cyrillic). The containment and policy checks
// compare path components; with path::generic_string() they THREW std::system_error ("No mapping for the Unicode character
// exists in the target multi-byte code page") on every machine whose ANSI page lacks a character - found by the first hosted CI
// run (English runner, Cyrillic folder). The names are spelled with \u escapes so that the source file's own encoding and the
// code page of the machine that compiles it play no part. Escapes out of the root must still be refused.
void testNonAnsiFolderAndFileNames_areAccepted_andEscapesStillRefused()
{
    const fs::path base = makeTempWorkspace() / ("nonansi-" + std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count()));
    const fs::path cjkRoot = base / fs::path(u8"项目-测试");
    const fs::path cyrillicDir = fs::path(u8"Проект");
    const fs::path cjkFile = fs::path(u8"文件夹") / fs::path(u8"文件.txt");
    writeFile(cjkRoot / cjkFile);
    writeFile(cjkRoot / cyrillicDir / "plain.txt");
    const auto policy = policyFor(cjkRoot);

    for (const fs::path& relative : {cjkFile, cyrillicDir / "plain.txt"}) {
        const auto accepted = app::WorkspacePathGuard::validatePath(relative, policy);
        assert(accepted.success() && "a folder or file name outside the ANSI code page must be accepted");
        assert(accepted.value().is_absolute());
        const std::string resolved = app::detail::asciiLower(app::detail::genericUtf8(accepted.value()));
        const std::string rootText = app::detail::asciiLower(app::detail::genericUtf8(fs::weakly_canonical(cjkRoot)));
        assert(resolved.starts_with(rootText) && "the resolved path must lie inside the workspace root");
    }

    // The key other components correlate paths with must not throw for such a name either, and must stay exact for it.
    assert(app::detail::normalizedPathKey(cjkFile) == app::detail::genericUtf8(cjkFile));
    assert(app::detail::normalizedPathKey(fs::path(u8"ABC") / cyrillicDir) == "abc/" + app::detail::genericUtf8(cyrillicDir));

    // The same checks still say no: leaving the root, a denied directory, an absolute path.
    const auto escaped = app::WorkspacePathGuard::validatePath(fs::path(u8"文件夹") / ".." / ".." / "outside.txt", policy);
    assert(!escaped.success() && escaped.error()->code == "workspace_path.unsafe");
    const auto denied = app::WorkspacePathGuard::validatePath(fs::path(u8"文件夹") / ".git" / "config", policy);
    assert(!denied.success() && denied.error()->code == "workspace_path.denied");
    const auto absolute = app::WorkspacePathGuard::validatePath(cjkRoot / cjkFile, policy);
    assert(!absolute.success() && absolute.error()->code == "workspace_path.unsafe");

    std::error_code ignored;
    fs::remove_all(base, ignored);
}

void testDeepNesting_returnsCanonical()
{
    const fs::path root = makeTempWorkspace();
    writeFile(root / "a" / "b" / "c" / "d.txt");
    const auto policy = policyFor(root);

    auto r = app::WorkspacePathGuard::validate("a/b/c/d.txt", policy);
    assert(r.success());
    assert(r.value().is_absolute());
    fs::remove_all(root);
}

// ---------------------------------------------------------------------------
// Group 2: Lexical denials — no real workspace needed
// ---------------------------------------------------------------------------

void testEmptyPath_unsafe()
{
    auto r = app::WorkspacePathGuard::validate("", lexicalPolicy());
    assert(!r.success());
    assert(r.error()->code == "workspace_path.unsafe");
}

void testDotDotAtRoot_unsafe()
{
    auto r = app::WorkspacePathGuard::validate("../etc/passwd", lexicalPolicy());
    assert(!r.success());
    assert(r.error()->code == "workspace_path.unsafe");
}

void testDotDotNested_unsafe()
{
    auto r = app::WorkspacePathGuard::validate("src/../../etc/passwd", lexicalPolicy());
    assert(!r.success());
    assert(r.error()->code == "workspace_path.unsafe");
}

void testAbsolutePathWindows_unsafe()
{
    auto r = app::WorkspacePathGuard::validate("C:\\windows\\system32\\cmd.exe", lexicalPolicy());
    assert(!r.success());
    assert(r.error()->code == "workspace_path.unsafe");
}

void testAbsolutePathUnix_unsafe()
{
    auto r = app::WorkspacePathGuard::validate("/etc/passwd", lexicalPolicy());
    assert(!r.success());
    assert(r.error()->code == "workspace_path.unsafe");
}

void testUncPathBackslash_unsafe()
{
    auto r = app::WorkspacePathGuard::validate("\\\\server\\share\\file.txt", lexicalPolicy());
    assert(!r.success());
    assert(r.error()->code == "workspace_path.unsafe");
}

void testUncPathForwardSlash_unsafe()
{
    auto r = app::WorkspacePathGuard::validate("//server/share/file.txt", lexicalPolicy());
    assert(!r.success());
    assert(r.error()->code == "workspace_path.unsafe");
}

void testDriveQualifiedPath_unsafe()
{
    // Drive letter + colon without backslash — still unsafe.
    auto r = app::WorkspacePathGuard::validate("D:file.txt", lexicalPolicy());
    assert(!r.success());
    assert(r.error()->code == "workspace_path.unsafe");
}

void testAlternateDataStream_unsafe()
{
    // ADS syntax file.txt:stream — colon must be rejected.
    auto r = app::WorkspacePathGuard::validate("file.txt:Zone.Identifier", lexicalPolicy());
    assert(!r.success());
    assert(r.error()->code == "workspace_path.unsafe");
}

void testAlternateDataStreamDoubleColon_unsafe()
{
    auto r = app::WorkspacePathGuard::validate("file.txt::$DATA", lexicalPolicy());
    assert(!r.success());
    assert(r.error()->code == "workspace_path.unsafe");
}

void testWindowsDeviceName_CON_unsafe()
{
    auto r = app::WorkspacePathGuard::validate("CON", lexicalPolicy());
    assert(!r.success());
    assert(r.error()->code == "workspace_path.unsafe");
}

void testWindowsDeviceName_COM1_unsafe()
{
    auto r = app::WorkspacePathGuard::validate("COM1", lexicalPolicy());
    assert(!r.success());
    assert(r.error()->code == "workspace_path.unsafe");
}

void testWindowsDeviceName_LPT3_unsafe()
{
    auto r = app::WorkspacePathGuard::validate("LPT3", lexicalPolicy());
    assert(!r.success());
    assert(r.error()->code == "workspace_path.unsafe");
}

void testWindowsDeviceName_NUL_unsafe()
{
    auto r = app::WorkspacePathGuard::validate("NUL", lexicalPolicy());
    assert(!r.success());
    assert(r.error()->code == "workspace_path.unsafe");
}

void testWindowsDeviceName_PRN_unsafe()
{
    auto r = app::WorkspacePathGuard::validate("PRN", lexicalPolicy());
    assert(!r.success());
    assert(r.error()->code == "workspace_path.unsafe");
}

void testNulByte_unsafe()
{
    std::string pathWithNul = "file";
    pathWithNul += '\0';
    pathWithNul += ".txt";
    auto r = app::WorkspacePathGuard::validate(pathWithNul, lexicalPolicy());
    assert(!r.success());
    assert(r.error()->code == "workspace_path.unsafe");
}

void testGitDirectory_denied()
{
    auto r = app::WorkspacePathGuard::validate(".git/config", lexicalPolicy());
    assert(!r.success());
    assert(r.error()->code == "workspace_path.denied");
}

void testGitDirectoryNested_denied()
{
    auto r = app::WorkspacePathGuard::validate("submodule/.git/HEAD", lexicalPolicy());
    assert(!r.success());
    assert(r.error()->code == "workspace_path.denied");
}

void testSshDirectory_denied()
{
    auto r = app::WorkspacePathGuard::validate(".ssh/known_hosts", lexicalPolicy());
    assert(!r.success());
    assert(r.error()->code == "workspace_path.denied");
}

void testAwsDirectory_denied()
{
    auto r = app::WorkspacePathGuard::validate(".aws/credentials", lexicalPolicy());
    assert(!r.success());
    assert(r.error()->code == "workspace_path.denied");
}

void testDotEnvFile_denied()
{
    auto r = app::WorkspacePathGuard::validate(".env", lexicalPolicy());
    assert(!r.success());
    assert(r.error()->code == "workspace_path.denied");
}

void testDotEnvLocalFile_denied()
{
    auto r = app::WorkspacePathGuard::validate(".env.local", lexicalPolicy());
    assert(!r.success());
    assert(r.error()->code == "workspace_path.denied");
}

void testPemExtension_denied()
{
    auto r = app::WorkspacePathGuard::validate("certs/server.pem", lexicalPolicy());
    assert(!r.success());
    assert(r.error()->code == "workspace_path.denied");
}

void testKeyExtension_denied()
{
    auto r = app::WorkspacePathGuard::validate("keys/private.key", lexicalPolicy());
    assert(!r.success());
    assert(r.error()->code == "workspace_path.denied");
}

void testPfxExtension_denied()
{
    auto r = app::WorkspacePathGuard::validate("certs/client.pfx", lexicalPolicy());
    assert(!r.success());
    assert(r.error()->code == "workspace_path.denied");
}

void testIdRsaFile_denied()
{
    auto r = app::WorkspacePathGuard::validate("id_rsa", lexicalPolicy());
    assert(!r.success());
    assert(r.error()->code == "workspace_path.denied");
}

void testIdEd25519File_denied()
{
    auto r = app::WorkspacePathGuard::validate("id_ed25519", lexicalPolicy());
    assert(!r.success());
    assert(r.error()->code == "workspace_path.denied");
}

void testCredentialsJsonFile_denied()
{
    auto r = app::WorkspacePathGuard::validate("credentials.json", lexicalPolicy());
    assert(!r.success());
    assert(r.error()->code == "workspace_path.denied");
}

void testSecretsJsonFile_denied()
{
    auto r = app::WorkspacePathGuard::validate("secrets.json", lexicalPolicy());
    assert(!r.success());
    assert(r.error()->code == "workspace_path.denied");
}

void testBuildDirectory_denied()
{
    auto r = app::WorkspacePathGuard::validate("build/output.exe", lexicalPolicy());
    assert(!r.success());
    assert(r.error()->code == "workspace_path.denied");
}

// ---------------------------------------------------------------------------
// Group 3: Trusted-policy validation errors
// ---------------------------------------------------------------------------

void testRelativeWorkspaceRoot_invalidRoot()
{
    app::WorkspaceReadPolicy policy;
    policy.workspaceRoot = "relative/path"; // not absolute
    policy.maxFileBytes  = policy.maxDirectoryEntries
                         = policy.maxSearchMatches
                         = policy.maxResultBytes = 1;

    auto r = app::WorkspacePathGuard::validate("file.txt", policy);
    assert(!r.success());
    assert(r.error()->code == "workspace_policy.invalid_root");
}

void testEmptyWorkspaceRoot_invalidRoot()
{
    app::WorkspaceReadPolicy policy;
    // workspaceRoot left default-constructed (empty)
    policy.maxFileBytes  = policy.maxDirectoryEntries
                         = policy.maxSearchMatches
                         = policy.maxResultBytes = 1;

    auto r = app::WorkspacePathGuard::validate("file.txt", policy);
    assert(!r.success());
    assert(r.error()->code == "workspace_policy.invalid_root");
}

void testZeroMaxFileBytes_invalidLimit()
{
#ifdef _WIN32
    app::WorkspaceReadPolicy policy = policyFor(fs::path("C:\\workspace"));
#else
    app::WorkspaceReadPolicy policy = policyFor(fs::path("/workspace"));
#endif
    policy.maxFileBytes = 0;

    auto r = app::WorkspacePathGuard::validate("file.txt", policy);
    assert(!r.success());
    assert(r.error()->code == "workspace_policy.invalid_limit");
}

void testAbsoluteAllowedRoot_invalidAllowedRoot()
{
#ifdef _WIN32
    auto policy = policyFor(fs::path("C:\\workspace"));
    policy.allowedRoots.push_back(fs::path("C:\\other")); // absolute — invalid
#else
    auto policy = policyFor(fs::path("/workspace"));
    policy.allowedRoots.push_back(fs::path("/other")); // absolute — invalid
#endif

    auto r = app::WorkspacePathGuard::validate("file.txt", policy);
    assert(!r.success());
    assert(r.error()->code == "workspace_policy.invalid_allowed_root");
}

void testDotDotDeniedPath_invalidDeniedPath()
{
#ifdef _WIN32
    auto policy = policyFor(fs::path("C:\\workspace"));
#else
    auto policy = policyFor(fs::path("/workspace"));
#endif
    policy.deniedPaths.push_back(fs::path("../outside")); // traversal — invalid

    auto r = app::WorkspacePathGuard::validate("file.txt", policy);
    assert(!r.success());
    assert(r.error()->code == "workspace_policy.invalid_denied_path");
}

// ---------------------------------------------------------------------------
// Group 4: Custom policy lists and allowed-roots enforcement
// ---------------------------------------------------------------------------

void testCustomDeniedPath_denied()
{
    auto policy = lexicalPolicy();
    policy.deniedPaths.push_back(fs::path("vendor/private"));

    auto r = app::WorkspacePathGuard::validate("vendor/private/secret.txt", policy);
    assert(!r.success());
    assert(r.error()->code == "workspace_path.denied");
}

void testCustomDeniedDirectoryName_denied()
{
    auto policy = lexicalPolicy();
    policy.deniedDirectoryNames.push_back("internal");

    auto r = app::WorkspacePathGuard::validate("src/internal/types.hpp", policy);
    assert(!r.success());
    assert(r.error()->code == "workspace_path.denied");
}

void testCustomDeniedExtension_denied()
{
    auto policy = lexicalPolicy();
    policy.deniedExtensions.push_back(".secret");

    auto r = app::WorkspacePathGuard::validate("data/passwords.secret", policy);
    assert(!r.success());
    assert(r.error()->code == "workspace_path.denied");
}

void testCustomDeniedFileName_denied()
{
    auto policy = lexicalPolicy();
    policy.deniedFileNames.push_back("shadow");

    auto r = app::WorkspacePathGuard::validate("etc/shadow", policy);
    assert(!r.success());
    assert(r.error()->code == "workspace_path.denied");
}

void testAllowedRoots_outsideAllowedRoots_denied()
{
    auto policy = lexicalPolicy();
    policy.allowedRoots.push_back(fs::path("src"));

    // "docs" is not inside "src".
    auto r = app::WorkspacePathGuard::validate("docs/readme.md", policy);
    assert(!r.success());
    assert(r.error()->code == "workspace_path.outside_allowed_roots");
}

void testAllowedRoots_insideAllowedRoot_success()
{
    const fs::path root = makeTempWorkspace();
    writeFile(root / "src" / "main.cpp");
    auto policy = policyFor(root);
    policy.allowedRoots.push_back(fs::path("src"));

    auto r = app::WorkspacePathGuard::validate("src/main.cpp", policy);
    assert(r.success());
    assert(r.value().is_absolute());
    fs::remove_all(root);
}

// ---------------------------------------------------------------------------
// Group 5: Error messages must not expose absolute paths or model input
// ---------------------------------------------------------------------------

void testErrorMessages_noAbsolutePaths()
{
    const auto policy = lexicalPolicy();

    // Probe strings that could leak path information if naively formatted.
    const std::vector<std::string> probes = {
        "",
        "../etc/passwd",
        "src/../../etc/passwd",
        "C:\\windows\\system32",
        "\\\\server\\share\\secret",
        "/etc/shadow",
        "D:file.txt",
        "file.txt:Zone.Identifier",
        "CON",
        "NUL",
        ".git/config",
        ".env",
        "id_rsa",
        "certs/server.pem",
        std::string("fi\0le.txt", 9),
    };

    for (const auto& probe : probes) {
        auto r = app::WorkspacePathGuard::validate(probe, policy);
        // All probes against lexicalPolicy() must fail.
        assert(!r.success());
        const std::string& msg = r.error()->message;
        // Message must not contain any absolute-path markers.
        assert(msg.find("C:\\") == std::string::npos);
        assert(msg.find("c:\\") == std::string::npos);
        assert(msg.find("\\\\") == std::string::npos);
        assert(msg.find("/etc") == std::string::npos);
        assert(msg.find("/shadow") == std::string::npos);
        assert(msg.find("/workspace") == std::string::npos);
        assert(msg.find("projects") == std::string::npos);
        // Message must not echo the probe itself.
        // (Only check non-empty probes to avoid a false positive on empty.)
        if (!probe.empty() && probe.find('\0') == std::string::npos)
            assert(msg.find(probe) == std::string::npos);
    }
}

} // namespace

int runWorkspacePathGuardTests()
{
    // Group 1: Basic success (real temp workspace)
    testCleanRelativePath_returnsAbsoluteCanonical();
    testNestedPath_returnsAbsoluteCanonical();
    testNonExistentFile_guardPassesExistenceCheckToExecutor();
    testIdempotent_samePathTwice();
    testNonAnsiFolderAndFileNames_areAccepted_andEscapesStillRefused();
    testDeepNesting_returnsCanonical();

    // Group 2: Lexical denials
    testEmptyPath_unsafe();
    testDotDotAtRoot_unsafe();
    testDotDotNested_unsafe();
    testAbsolutePathWindows_unsafe();
    testAbsolutePathUnix_unsafe();
    testUncPathBackslash_unsafe();
    testUncPathForwardSlash_unsafe();
    testDriveQualifiedPath_unsafe();
    testAlternateDataStream_unsafe();
    testAlternateDataStreamDoubleColon_unsafe();
    testWindowsDeviceName_CON_unsafe();
    testWindowsDeviceName_COM1_unsafe();
    testWindowsDeviceName_LPT3_unsafe();
    testWindowsDeviceName_NUL_unsafe();
    testWindowsDeviceName_PRN_unsafe();
    testNulByte_unsafe();
    testGitDirectory_denied();
    testGitDirectoryNested_denied();
    testSshDirectory_denied();
    testAwsDirectory_denied();
    testDotEnvFile_denied();
    testDotEnvLocalFile_denied();
    testPemExtension_denied();
    testKeyExtension_denied();
    testPfxExtension_denied();
    testIdRsaFile_denied();
    testIdEd25519File_denied();
    testCredentialsJsonFile_denied();
    testSecretsJsonFile_denied();
    testBuildDirectory_denied();

    // Group 3: Policy validation errors
    testRelativeWorkspaceRoot_invalidRoot();
    testEmptyWorkspaceRoot_invalidRoot();
    testZeroMaxFileBytes_invalidLimit();
    testAbsoluteAllowedRoot_invalidAllowedRoot();
    testDotDotDeniedPath_invalidDeniedPath();

    // Group 4: Custom policy lists and allowed-roots
    testCustomDeniedPath_denied();
    testCustomDeniedDirectoryName_denied();
    testCustomDeniedExtension_denied();
    testCustomDeniedFileName_denied();
    testAllowedRoots_outsideAllowedRoots_denied();
    testAllowedRoots_insideAllowedRoot_success();

    // Group 5: Error messages — no leaks
    testErrorMessages_noAbsolutePaths();

    return 0;
}
