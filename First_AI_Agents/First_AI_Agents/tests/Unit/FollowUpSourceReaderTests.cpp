// PC-5C-2a: the filesystem file loader, the candidate-path rules and the production source reader. The loader runs against a REAL
// temporary workspace (including real links where the machine lets the test create them); the reader runs over in-memory repositories;
// and the last test drives the whole composer (quote, authorize, claim, plan) through the production reader and loader, with a
// scripted provider, to prove that what they produce is what the assembler accepts.

#include "FollowUpServiceTestSupport.hpp"

#include "ai/application/FilesystemFollowUpFileLoader.hpp"
#include "ai/application/planning/FollowUpSourceReader.hpp"
#include "ai/application/InMemoryManifestWorkLedgerRepository.hpp"
#include "ai/application/InMemoryOutcomeBriefRepository.hpp"
#include "ai/application/FilesystemPathUtils.hpp"
#include "ai/application/WorkspacePathGuard.hpp"
#include "ai/application/WorkspaceReadPolicy.hpp"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <new>

// ---- an allocation probe ----------------------------------------------------------------------------------------------------------
// The test binary replaces the global allocator to COUNT allocations whose size falls in a window, while a probe is alive. It changes
// nothing else: every allocation is still a plain malloc.
namespace allocation_probe {
std::atomic<bool> active {false};
std::atomic<std::size_t> low {0};
std::atomic<std::size_t> high {0};
std::atomic<int> hits {0};
} // namespace allocation_probe

void* operator new(std::size_t size)
{
    if (allocation_probe::active.load(std::memory_order_relaxed) && size >= allocation_probe::low.load() && size <= allocation_probe::high.load()) {
        allocation_probe::hits.fetch_add(1);
    }
    if (void* memory = std::malloc(size == 0 ? 1 : size)) return memory;
    throw std::bad_alloc();
}
void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }

using namespace ai::application;
using namespace followup_service_test;
namespace common = ai::common;
namespace fs = std::filesystem;

namespace {

// Counts the allocations of `low`..`high` bytes made while it is alive (one at a time).
class AllocationProbe final {
public:
    AllocationProbe(std::size_t low, std::size_t high)
    {
        allocation_probe::hits = 0;
        allocation_probe::low = low;
        allocation_probe::high = high;
        allocation_probe::active = true;
    }
    ~AllocationProbe() { allocation_probe::active = false; }
    AllocationProbe(const AllocationProbe&) = delete;
    AllocationProbe& operator=(const AllocationProbe&) = delete;
    [[nodiscard]] int hits() const
    {
        allocation_probe::active = false;
        return allocation_probe::hits.load();
    }
};

} // namespace

namespace {

using Safety = FollowUpFileSafety;

// "src/", four Cyrillic letters (U+0422 U+0435 U+0441 U+0442), ".cpp" - as UTF-8 bytes built from \u escapes, so the test depends neither
// on the compiler's source charset nor on the ANSI code page of the machine that compiles or runs it.
std::string utf8Bytes(const std::u8string& text)
{
    return std::string(reinterpret_cast<const char*>(text.data()), text.size());
}
const std::string kUnicodePath = std::string("src/") + utf8Bytes(u8"\u0422\u0435\u0441\u0442") + ".cpp";
// ---- a real temporary workspace -------------------------------------------------------------------------------------------------

class TemporaryDirectory final {
public:
    explicit TemporaryDirectory(const std::string& name)
        : path_(fs::temp_directory_path() / ("ai-orchestrator-pc5c2a-" + name))
    {
        std::error_code ignored;
        fs::remove_all(path_, ignored);
        std::error_code error;
        if (!fs::create_directories(path_, error) || error) throw std::runtime_error("Could not create the test workspace");
    }
    ~TemporaryDirectory()
    {
        std::error_code ignored;
        fs::remove_all(path_, ignored);
    }
    TemporaryDirectory(const TemporaryDirectory&) = delete;
    TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;

    [[nodiscard]] const fs::path& path() const { return path_; }

    void write(const std::string& utf8Relative, const std::string& content) const
    {
        const fs::path absolute = path_ / ai::application::detail::pathFromUtf8(utf8Relative);
        std::error_code error;
        fs::create_directories(absolute.parent_path(), error);
        std::ofstream stream(absolute, std::ios::binary | std::ios::trunc);
        stream << content;
        if (!stream) throw std::runtime_error("Could not write a test file: " + utf8Relative);
    }

private:
    fs::path path_;
};

// Ordinary source-looking text of exactly `size` bytes (a long uniform run would look like a token to the secret scanner).
std::string filler(std::size_t size)
{
    const std::string line = "// filler line of an ordinary source file\n";
    std::string text;
    while (text.size() < size) text += line;
    text.resize(size);
    return text;
}

const FollowUpFileCandidate* find(const std::vector<FollowUpFileCandidate>& files, const std::string& path)
{
    for (const auto& file : files) {
        if (file.relativePath == path) return &file;
    }
    return nullptr;
}

Safety safetyOf(const std::vector<FollowUpFileCandidate>& files, const std::string& path)
{
    const auto* file = find(files, path);
    check(file != nullptr, "the loader returned a candidate for " + path);
    return file->safety;
}

// ---- the candidate paths --------------------------------------------------------------------------------------------------------

ProposedFileChange change(const std::string& path, FileChangeOperation operation = FileChangeOperation::Modify)
{
    ProposedFileChange value;
    value.operation = operation;
    value.relativePath = ai::application::detail::pathFromUtf8(path);
    return value;
}

void testTheCandidatePathsAreCanonicalUniqueOrderedAndBounded()
{
    std::vector<ProposedFileChange> changes = {change("src/b.cpp"), change("src/a.cpp"), change("src/a.cpp"), change("docs/readme.md", FileChangeOperation::Create),
        change("old/gone.cpp", FileChangeOperation::Delete), change("pages/[id].tsx"), change(kUnicodePath)};
    // A rename names the file it produces; its source is not a candidate.
    auto rename = change("src/new.cpp", FileChangeOperation::Rename);
    rename.sourcePath = fs::path("src/older.cpp");
    changes.push_back(rename);
    const auto paths = followUpCandidatePathsOf(changes);
    check(paths == (std::vector<std::string> {"docs/readme.md", "old/gone.cpp", "pages/[id].tsx", "src/a.cpp", "src/b.cpp", "src/new.cpp", kUnicodePath}),
        "Every change of every operation is a candidate once, spelled with '/', bytewise ordered; a rename's source is not");

    // What the assembler would refuse as a loader bug is not a candidate.
    const std::vector<ProposedFileChange> odd = {change("../escape.cpp"), change("/abs/file.cpp"), change("a//b.cpp"), change("a/./b.cpp"),
        change("a/b.cpp/"), change("C:/x.cpp"), change("what?.cpp"), change(""), change("fine.cpp")};
    check(followUpCandidatePathsOf(odd) == std::vector<std::string> {"fine.cpp"}, "Non-canonical spellings are left out, not refused");

    // The workspace's case rule: where it is case-insensitive two spellings are one file and the bytewise smallest wins.
    const auto spellings = followUpCandidatePathsOf({change("src/a.cpp"), change("Src/A.cpp")});
#ifdef _WIN32
    check(spellings == std::vector<std::string> {"Src/A.cpp"}, "Two spellings of one file collapse to the smallest where the workspace ignores case");
#else
    check(spellings == (std::vector<std::string> {"Src/A.cpp", "src/a.cpp"}), "Where the workspace is case-sensitive both stay");
#endif

    // The list is NEVER cut short here: the bound is the reader's to enforce, as a refusal (see the reader tests).
    std::vector<ProposedFileChange> many;
    for (std::size_t index = 0; index < kFollowUpMaximumCandidates + 40; ++index) {
        std::string number = std::to_string(index);
        many.push_back(change("f/" + std::string(4 - number.size(), '0') + number + ".cpp"));
    }
    const auto all = followUpCandidatePathsOf(many);
    check(all.size() == kFollowUpMaximumCandidates + 40 && all.front() == "f/0000.cpp" && all.back() == "f/0295.cpp",
        "Every unique candidate is returned, in order - nothing is silently dropped");
    check(followUpCandidatePathsOf({}).empty(), "No changes, no candidates");
    // The order is BYTEWISE on the spelling, not on the case-folded identity: an upper-case name sorts before a lower-case one.
    check(followUpCandidatePathsOf({change("a.cpp"), change("B.cpp")}) == (std::vector<std::string> {"B.cpp", "a.cpp"}),
        "Candidates are ordered bytewise by their spelling");
}

// ---- the loader -----------------------------------------------------------------------------------------------------------------

// Makes a real directory link inside `root`: a symbolic link, or where that needs a privilege this machine does not grant, a junction.
bool makeDirectoryLink(const fs::path& link, const fs::path& target)
{
    std::error_code error;
    fs::create_directory_symlink(target, link, error);
    if (!error) return true;
#ifdef _WIN32
    const std::string command = "mklink /J \"" + link.string() + "\" \"" + target.string() + "\" >nul 2>&1";
    return std::system(("cmd /c " + command).c_str()) == 0 && fs::exists(link);
#else
    return false;
#endif
}

bool makeFileLink(const fs::path& link, const fs::path& target)
{
    std::error_code error;
    fs::create_symlink(target, link, error);
    return !error;
}

void testTheLoaderClassifiesEveryFileBeforeAByteCanLeave()
{
    TemporaryDirectory workspace("loader");
    workspace.write("src/ok.cpp", "int ok() { return 1; }\n");
    workspace.write("src/pages/[id].tsx", "export default 1;\n");
    workspace.write(kUnicodePath, "// unicode name\n");
    workspace.write(".env", "TOKEN=abc\n");
    workspace.write("certs/server.pem", "not really a key\n");
    workspace.write(".git/config", "[core]\n");
    workspace.write("build/out.txt", "generated\n");
    workspace.write("bin/data.bin", std::string("abc\0def", 7));
    workspace.write("text/latin1.txt", std::string("caf\xE9 au lait\n"));
    workspace.write("text/secret.txt", "-----BEGIN RSA PRIVATE KEY-----\nMIIBOgIBAAJBAKj34GkxFhD90vcNLYLInFEX6Ppy1tPf9Cnzj4p4WGeKLs1Pt8Qu\n-----END RSA PRIVATE KEY-----\n");
    workspace.write("big/exact.txt", filler(kFollowUpComposerMaximumCandidateBytes));
    workspace.write("big/over.txt", filler(kFollowUpComposerMaximumCandidateBytes + 1));
    std::error_code error;
    fs::create_directories(workspace.path() / "adir", error);

    const std::vector<std::string> paths = {"adir", "big/exact.txt", "big/over.txt", "bin/data.bin", "build/out.txt", "certs/server.pem",
        "gone/missing.cpp", ".env", ".git/config", kUnicodePath, "src/ok.cpp", "src/pages/[id].tsx", "text/latin1.txt", "text/secret.txt"};
    FilesystemFollowUpFileLoader loader;
    const auto loaded = loader.load(workspace.path(), paths);
    check(loaded.success(), "the loader runs: " + codeOf(loaded.error()));
    const auto& files = loaded.value();
    check(files.size() == paths.size(), "One candidate per input path");
    for (std::size_t index = 0; index < paths.size(); ++index) {
        check(files[index].relativePath == paths[index], "...in the input order: " + paths[index]);
        check(!files[index].selected, "...none of them selected: " + paths[index]);
    }

    check(safetyOf(files, "src/ok.cpp") == Safety::Eligible, "An ordinary source file is eligible");
    check(find(files, "src/ok.cpp")->content == "int ok() { return 1; }\n" && find(files, "src/ok.cpp")->contentSha256 == sha256Hex("int ok() { return 1; }\n"),
        "...with its complete bytes and the sha256 of exactly those bytes");
    check(safetyOf(files, "src/pages/[id].tsx") == Safety::Eligible, "A route file with brackets is an ordinary file");
    // The read policy, the path guard and the secret scan compare and scan names by their UTF-8 text, never through the narrow system code
    // page. That conversion used to THROW for a name the page cannot hold, and the file was then excluded as "unvettable" - on some
    // machines (an English Windows) and not on others (this test's earlier either-or). Now the name is vetted like any other: the file is
    // read as the file it names, on every machine.
    const auto unicodeVerdict = safetyOf(files, kUnicodePath);
    check(unicodeVerdict == Safety::Eligible,
        "A non-ASCII file name is vetted and read as the file it names (verdict: " + std::string(toString(unicodeVerdict)) + ")");
    check(find(files, kUnicodePath)->content == "// unicode name\n", "...and it is the right file");
    check(safetyOf(files, "big/exact.txt") == Safety::Eligible && find(files, "big/exact.txt")->content.size() == kFollowUpComposerMaximumCandidateBytes,
        "A file of exactly 256 KiB is eligible");
    check(safetyOf(files, "big/over.txt") == Safety::TooLarge, "One byte more is too large, and was never read");
    check(safetyOf(files, ".env") == Safety::UnsafePath && safetyOf(files, "certs/server.pem") == Safety::UnsafePath
            && safetyOf(files, ".git/config") == Safety::UnsafePath && safetyOf(files, "build/out.txt") == Safety::UnsafePath,
        "Credential names, key extensions, .git and build directories are denied by the read policy");
    check(safetyOf(files, "bin/data.bin") == Safety::Binary, "A NUL byte makes a file binary");
    check(safetyOf(files, "text/latin1.txt") == Safety::InvalidUtf8, "A file that is not UTF-8 is never sent");
    check(safetyOf(files, "text/secret.txt") == Safety::SecretDetected, "A file that holds a secret is never sent");
    check(safetyOf(files, "gone/missing.cpp") == Safety::ReadFailed && safetyOf(files, "adir") == Safety::ReadFailed,
        "A missing file and a directory are not readable files");
    for (const auto& file : files) {
        if (file.safety == Safety::Eligible) continue;
        check(file.content.empty() && file.contentSha256.empty(), "Nothing of an excluded file is kept: " + file.relativePath);
    }
}

void testTheLoaderNeverFollowsALink()
{
    TemporaryDirectory workspace("links");
    TemporaryDirectory outside("outside");
    workspace.write("real/inside.txt", "inside\n");
    outside.write("secret.txt", "outside\n");
    FilesystemFollowUpFileLoader loader;

    bool exercised = false;
    bool exercisedInside = false;
    bool exercisedOutside = false;
    bool exercisedFile = false;
    // A directory link inside the workspace that points at another directory inside it, and one that points outside.
    if (makeDirectoryLink(workspace.path() / "linkdir", workspace.path() / "real")) {
        exercised = exercisedInside = true;
        const auto loaded = loader.load(workspace.path(), {"linkdir/inside.txt", "real/inside.txt"});
        check(loaded.success(), "the loader runs");
        check(safetyOf(loaded.value(), "linkdir/inside.txt") == Safety::Symlink, "A file reached through a directory link is a symlink, even inside the root");
        check(safetyOf(loaded.value(), "real/inside.txt") == Safety::Eligible, "...while the real path of the same file is fine");
    }
    if (makeDirectoryLink(workspace.path() / "escape", outside.path())) {
        exercised = exercisedOutside = true;
        const auto loaded = loader.load(workspace.path(), {"escape/secret.txt"});
        const auto safety = safetyOf(loaded.value(), "escape/secret.txt");
        check(safety == Safety::Symlink || safety == Safety::UnsafePath, "A link out of the workspace is never read");
        check(find(loaded.value(), "escape/secret.txt")->content.empty(), "...and nothing of the file is kept");
    }
    if (makeFileLink(workspace.path() / "filelink.txt", workspace.path() / "real" / "inside.txt")) {
        exercised = exercisedFile = true;
        const auto loaded = loader.load(workspace.path(), {"filelink.txt"});
        check(safetyOf(loaded.value(), "filelink.txt") == Safety::Symlink, "A file that is itself a link is a symlink");
    }
    // A machine that lets a test create neither a symbolic link nor a junction cannot exercise this branch; say so instead of
    // pretending it was proven.
    if (!exercised) std::cout << "NOTE: this machine allows no symbolic link and no junction here; the link checks were NOT exercised\n";
    else std::cout << "links exercised: inside-link=" << exercisedInside << " outside-link=" << exercisedOutside << " file-link=" << exercisedFile << "\n";
}

// The TOCTOU window, closed and exercised through the loader's two test hooks: what an attacker does BETWEEN the path checks and the
// open (beforeOpen), and what is attempted once the handle is open (afterOpen). Deterministic: the hooks run on the loader's own thread
// at exactly those points.
void testASwapInTheGapNeverChangesWhatIsRead()
{
    TemporaryDirectory workspace("swap");
    TemporaryDirectory outside("swap-outside");
    workspace.write("src/a.txt", "original a\n");
    workspace.write("dir/b.txt", "original b\n");
    outside.write("a.txt", "OUTSIDE a\n");
    outside.write("b.txt", "OUTSIDE b\n");

    // 1. The regular file is replaced by a link to another file between the checks and the open.
    {
        int beforeCalls = 0;
        bool swapped = false;
        FollowUpFileLoaderHooks hooks;
        hooks.beforeOpen = [&](const fs::path& actual) {
            ++beforeCalls;
            std::error_code ignored;
            fs::remove(actual, ignored);
            swapped = makeFileLink(actual, outside.path() / "a.txt");
        };
        const FilesystemFollowUpFileLoader loader(hooks);
        const auto loaded = loader.load(workspace.path(), {"src/a.txt"});
        check(loaded.success() && beforeCalls == 1, "the hook ran once, in the gap");
        if (swapped) {
            const auto* file = find(loaded.value(), "src/a.txt");
            check(file != nullptr && file->safety == Safety::Symlink && file->content.empty() && file->contentSha256.empty(),
                "A file swapped for a link in the gap is a symlink and none of the target's bytes is kept");
            std::cout << "swap file->link exercised\n";
        } else {
            std::cout << "NOTE: no file link could be created here; the file swap was NOT exercised\n";
        }
        std::error_code ignored;
        fs::remove(workspace.path() / "src" / "a.txt", ignored);
        workspace.write("src/a.txt", "original a\n");
    }

    // 2. A directory ABOVE the file is replaced by a link to a directory that holds a file of the same name.
    {
        int beforeCalls = 0;
        bool swapped = false;
        FollowUpFileLoaderHooks hooks;
        hooks.beforeOpen = [&](const fs::path& actual) {
            ++beforeCalls;
            std::error_code error;
            fs::rename(actual.parent_path(), workspace.path() / "dir.moved", error);
            if (!error) swapped = makeDirectoryLink(workspace.path() / "dir", outside.path());
        };
        const FilesystemFollowUpFileLoader loader(hooks);
        const auto loaded = loader.load(workspace.path(), {"dir/b.txt"});
        check(loaded.success() && beforeCalls == 1, "the hook ran once, in the gap");
        if (swapped) {
            const auto* file = find(loaded.value(), "dir/b.txt");
            check(file != nullptr && file->safety != Safety::Eligible && file->content.empty() && file->contentSha256.empty(),
                "A directory swapped for a link in the gap never hands over the bytes of the other directory's file");
            check(file != nullptr && file->safety == Safety::Symlink, "...it is caught by the handle's real path: a symlink");
            std::cout << "swap directory->link exercised\n";
        } else {
            std::cout << "NOTE: no directory link could be created here; the directory swap was NOT exercised\n";
        }
    }
}

// With the handle open the file can be neither written, renamed nor deleted on Windows (read-only sharing); on POSIX a change cannot
// reach the bytes of the open handle. Either way the bytes the loader keeps are the bytes of the file it opened, never a mix.
void testWhatIsDoneWhileTheHandleIsOpenNeverChangesTheBytes()
{
    TemporaryDirectory workspace("open-handle");
    const std::string original = "original text\n";
    workspace.write("src/a.txt", original);
    const fs::path file = workspace.path() / "src" / "a.txt";

    int afterCalls = 0;
    bool appended = false;
    bool renamed = false;
    bool removed = false;
    FollowUpFileLoaderHooks hooks;
    hooks.afterOpen = [&](const fs::path& actual) {
        ++afterCalls;
        {
            std::ofstream out(actual, std::ios::binary | std::ios::app);
            if (out) {
                out << "grown\n";
                out.flush();
                appended = static_cast<bool>(out);
            }
        }
        std::error_code error;
        fs::rename(actual, actual.parent_path() / "moved.txt", error);
        renamed = !error;
        if (!renamed) {
            error.clear();
            removed = fs::remove(actual, error) && !error;
        }
    };
    const FilesystemFollowUpFileLoader loader(hooks);
    const auto loaded = loader.load(workspace.path(), {"src/a.txt"});
    check(loaded.success() && afterCalls == 1, "the hook ran once, with the handle open");
    const auto* candidate = find(loaded.value(), "src/a.txt");
    check(candidate != nullptr, "the candidate is there");
#ifdef _WIN32
    check(!appended && !renamed && !removed, "While the loader holds the file it cannot be written, renamed or deleted");
    check(candidate->safety == Safety::Eligible && candidate->content == original && candidate->contentSha256 == sha256Hex(original),
        "...and the bytes are exactly the original ones");
    std::ifstream after(file, std::ios::binary);
    const std::string onDisk((std::istreambuf_iterator<char>(after)), std::istreambuf_iterator<char>());
    check(onDisk == original, "...the file on disk is untouched");
#else
    // POSIX: the change may happen, and then the handle still reads one consistent file or the file is refused.
    check(candidate->safety != Safety::Eligible
            || (candidate->contentSha256 == sha256Hex(candidate->content)
                && (candidate->content == original || candidate->content == original + "grown\n")),
        "A change made with the handle open never produces a mixed or unsealed content");
#endif
}

void testTheLoaderRefusesAnUnavailableWorkspace()
{
    FilesystemFollowUpFileLoader loader;
    requireCode(loader.load(fs::path("relative/root"), {"a.cpp"}), "follow_up.workspace_unavailable", "a relative root");
    requireCode(loader.load(fs::path(), {"a.cpp"}), "follow_up.workspace_unavailable", "an empty root");
    requireCode(loader.load(fs::temp_directory_path() / "ai-orchestrator-pc5c2a-does-not-exist", {"a.cpp"}), "follow_up.workspace_unavailable",
        "a root that does not exist");
    TemporaryDirectory directory("notadir");
    directory.write("afile", "x");
    requireCode(loader.load(directory.path() / "afile", {"a.cpp"}), "follow_up.workspace_unavailable", "a root that is a file");
    const auto empty = loader.load(directory.path(), {});
    check(empty.success() && empty.value().empty(), "No paths, no candidates");
}

// ---- the production reader ------------------------------------------------------------------------------------------------------

class ScriptedProposals final : public IChangeProposalRepository {
public:
    common::Result<void> insertChangeSet(const ProposedChangeSet&) override { return unexpected<void>(); }
    common::Result<ProposedChangeSet> updateChangeSetState(const ChangeSetId&, ChangeSetState) override { return unexpected<ProposedChangeSet>(); }
    common::Result<void> markMissingEvidence(const ChangeSetId&, std::string, std::string) override { return unexpected<void>(); }
    [[nodiscard]] common::Result<std::optional<ProposedChangeSet>> findChangeSet(const ChangeSetId&) const override { return unexpected<std::optional<ProposedChangeSet>>(); }
    [[nodiscard]] common::Result<std::vector<ProposedChangeSet>> listForRun(const RunId& runId) const override
    {
        ++reads;
        if (failure.has_value()) return common::Result<std::vector<ProposedChangeSet>>::failure(*failure);
        std::vector<ProposedChangeSet> sets;
        const auto found = byRun.find(runId);
        if (found != byRun.end()) sets = found->second;
        return common::Result<std::vector<ProposedChangeSet>>::ok(std::move(sets));
    }
    [[nodiscard]] common::Result<std::vector<ProposedChangeSet>> listForProject(const ProjectId&) const override { return unexpected<std::vector<ProposedChangeSet>>(); }
    [[nodiscard]] common::Result<std::vector<ProposedFileChange>> listChanges(const ChangeSetId&) const override { return unexpected<std::vector<ProposedFileChange>>(); }
    [[nodiscard]] common::Result<std::vector<ValidationFinding>> listValidationFindings(const ChangeSetId&) const override { return unexpected<std::vector<ValidationFinding>>(); }
    [[nodiscard]] common::Result<std::unordered_map<ChangeSetId, std::vector<ProposedFileChange>>> listChangesForSets(
        const std::vector<ChangeSetId>& ids) const override
    {
        std::unordered_map<ChangeSetId, std::vector<ProposedFileChange>> grouped;
        for (const auto& [run, sets] : byRun) {
            static_cast<void>(run);
            for (const auto& set : sets) {
                if (std::find(ids.begin(), ids.end(), set.changeSetId) != ids.end()) grouped[set.changeSetId] = set.changes;
            }
        }
        return common::Result<std::unordered_map<ChangeSetId, std::vector<ProposedFileChange>>>::ok(std::move(grouped));
    }
    [[nodiscard]] common::Result<std::unordered_map<ChangeSetId, std::vector<ValidationFinding>>> listValidationFindingsForSets(
        const std::vector<ChangeSetId>&) const override
    {
        return unexpected<std::unordered_map<ChangeSetId, std::vector<ValidationFinding>>>();
    }

    void add(const RunId& runId, const std::string& id, std::vector<ProposedFileChange> changes)
    {
        ProposedChangeSet set;
        set.changeSetId = id;
        set.runId = runId;
        set.projectId = kProject;
        set.changes = std::move(changes);
        byRun[runId].push_back(std::move(set));
    }

    std::map<RunId, std::vector<ProposedChangeSet>> byRun;
    std::optional<common::Error> failure;
    mutable int reads {0};

private:
    template <typename T>
    static common::Result<T> unexpected() { return common::Result<T>::failure({"test.unexpected_call", "The reader must only read run proposals"}); }
};

class ScriptedResolutions final : public IManifestVerificationManualHandoffResolutionReader {
public:
    common::Result<std::vector<ManifestVerificationManualHandoffRunResolution>> resolutionsForRuns(const std::vector<Run>& runs) const override
    {
        std::vector<ManifestVerificationManualHandoffRunResolution> result;
        for (const auto& run : runs) result.push_back({run.id, ManifestVerificationManualHandoffResolution::NotApplicable});
        return common::Result<std::vector<ManifestVerificationManualHandoffRunResolution>>::ok(std::move(result));
    }
};

class ScriptedMemory final : public IProjectMemoryRepository {
public:
    common::Result<ProjectMemorySegment> insertMemorySegment(ProjectMemorySegment) override { return common::Result<ProjectMemorySegment>::failure({"test.write_forbidden", "no"}); }
    common::Result<std::vector<ProjectMemorySegment>> listMemorySegments(const ProjectId&) const override { return common::Result<std::vector<ProjectMemorySegment>>::ok({}); }
    common::Result<std::optional<ProjectMemorySegment>> findMemorySegment(const ProjectMemorySegmentId&) const override { return common::Result<std::optional<ProjectMemorySegment>>::ok(std::nullopt); }
    common::Result<void> removeMemorySegments(const ProjectId&) override { return common::Result<void>::failure({"test.write_forbidden", "no"}); }
};

// Counts what the loader is asked, and delegates to the real filesystem loader.
class CountingLoader final : public IFollowUpFileLoader {
public:
    [[nodiscard]] common::Result<std::vector<FollowUpFileCandidate>> load(
        const fs::path& root, const std::vector<std::string>& paths) const override
    {
        ++calls;
        lastRoot = root;
        return inner.load(root, paths);
    }
    FilesystemFollowUpFileLoader inner;
    mutable int calls {0};
    mutable fs::path lastRoot;
};

// A project with a workspace on disk, one Completed manifest Run with its Outcome head and two proposals, and the real snapshot service.
struct Fixture {
    TemporaryDirectory projectRoot {"project-root"};
    TemporaryDirectory frozenWorkspace {"frozen-workspace"};
    InMemoryProjectRepository projects;
    InMemoryRunRepository runs;
    InMemoryOutcomeBriefRepository briefs {[this](const RunId& id) -> std::optional<OutcomeBriefRunOwner> {
        const auto run = runs.findRun(id);
        if (!run.success() || !run.value().has_value()) return std::nullopt;
        return OutcomeBriefRunOwner {run.value()->projectId, run.value()->status};
    }};
    InMemoryManifestWorkLedgerRepository ledger;
    ScriptedResolutions resolutions;
    ScriptedMemory memory;
    ProjectContextSnapshotService snapshots {projects, runs, ledger, resolutions, memory};
    ScriptedProposals proposals;
    CountingLoader loader;
    FollowUpSourceReader reader {projects, runs, briefs, snapshots, proposals, loader};

    explicit Fixture(bool frozen = true)
    {
        Project project;
        project.id = kProject;
        project.name = "Demo";
        project.rootPath = projectRoot.path();
        project.status = ProjectStatus::Active;
        check(projects.insert(project).success(), "setup: project");
        Run run;
        run.id = kRun;
        run.projectId = kProject;
        run.title = "Run";
        run.task = "Task";
        run.workflowId = kExecutionManifestWorkflowId;
        run.status = RunStatus::Completed;
        if (frozen) run.workspacePath = frozenWorkspace.path();
        check(runs.insertRun(run).success(), "setup: run");
        check(briefs.append(briefOf(kRun, kProject), OutcomeBriefExpectedHead::none()).success(), "setup: outcome brief");
        // The Run's own workspace and the project root hold DIFFERENT content for the same path: the reader must use the Run's.
        frozenWorkspace.write("src/a.cpp", "// frozen workspace\nint a() { return 1; }\n");
        frozenWorkspace.write("src/b.cpp", "int b() { return 2; }\n");
        frozenWorkspace.write(".env", "TOKEN=abc\n");
        projectRoot.write("src/a.cpp", "// project root, NOT the Run's workspace\n");
        proposals.add(kRun, "set-1", {change("src/b.cpp"), change("src/a.cpp")});
        proposals.add(kRun, "set-2", {change(".env", FileChangeOperation::Create), change("src/a.cpp")});
    }

    void setProjectStatus(ProjectStatus status)
    {
        auto project = projects.findById(kProject).value().value();
        project.status = status;
        check(projects.update(project).success(), "setup: project status");
    }
};

void testTheReaderGivesTheMaterialOfTheSourceRun()
{
    Fixture f;
    const auto read = f.reader.read(kProject, kRun);
    check(read.success(), "the reader reads: " + codeOf(read.error()));
    const auto& material = read.value();
    check(material.project.id == kProject && material.project.status == ProjectStatus::Active && material.runProjectId == kProject,
        "The project row and the project the Run row names");
    check(material.brief.runId == kRun && material.brief.sourceStateFingerprint == briefOf(kRun, kProject).sourceStateFingerprint,
        "The CURRENT Outcome head");
    check(material.snapshot.selectedRunId == std::optional<std::string>(kRun) && material.snapshot.projectId == kProject && !material.snapshot.fingerprint.empty(),
        "The PC-1 snapshot of the source Run");
    check(material.candidates.size() == 3 && material.candidates[0].relativePath == ".env" && material.candidates[1].relativePath == "src/a.cpp"
            && material.candidates[2].relativePath == "src/b.cpp",
        "The candidates are the files of the Run's proposals, once each, in path order");
    check(material.candidates[0].safety == Safety::UnsafePath && material.candidates[1].safety == Safety::Eligible && material.candidates[2].safety == Safety::Eligible,
        "...classified by the loader (the credential file is denied)");
    check(material.candidates[1].content == "// frozen workspace\nint a() { return 1; }\n",
        "The files come from the Run's FROZEN workspace, not from the project root");
    check(f.loader.lastRoot == f.frozenWorkspace.path(), "...which is the root the loader was given");

    // Without a frozen workspace the project root is the root (the snapshot's own rule).
    Fixture plain(false);
    plain.projectRoot.write("src/b.cpp", "int b() { return 22; }\n");
    plain.projectRoot.write("src/a.cpp", "int a() { return 11; }\n");
    const auto fallback = plain.reader.read(kProject, kRun);
    check(fallback.success() && plain.loader.lastRoot == plain.projectRoot.path() && fallback.value().candidates[1].content == "int a() { return 11; }\n",
        "A Run without a frozen workspace is read from the project root");
}

void testTheReaderReadsNothingItWasNotEntitledTo()
{
    {
        Fixture f;
        f.setProjectStatus(ProjectStatus::Archived);
        const auto read = f.reader.read(kProject, kRun);
        check(read.success() && read.value().project.status == ProjectStatus::Archived, "An archived project is reported as it is");
        check(read.value().snapshot.fingerprint.empty() && read.value().candidates.empty(), "...with no snapshot and no candidates");
        check(f.loader.calls == 0 && f.proposals.reads == 0, "...and no workspace file and no proposal was read");
    }
    {
        Fixture f;
        auto run = f.runs.findRun(kRun).value().value();
        run.projectId = "project-2";
        check(f.runs.updateRun(run).success(), "setup: the Run moved");
        const auto read = f.reader.read(kProject, kRun);
        check(read.success() && read.value().runProjectId == "project-2" && read.value().candidates.empty() && read.value().snapshot.fingerprint.empty()
                && f.loader.calls == 0,
            "A Run of another project is reported with the project its row names, and nothing of its workspace is read");
    }
}

// `count` more candidate paths for the Run (none of them exists on disk: each one is an ineligible candidate, which is all the count test needs).
void addCandidates(Fixture& f, const std::string& setId, std::size_t count)
{
    std::vector<ProposedFileChange> more;
    for (std::size_t index = 0; index < count; ++index) {
        std::string number = std::to_string(index);
        more.push_back(change("gen/" + setId + "-" + std::string(4 - number.size(), '0') + number + ".txt"));
    }
    f.proposals.add(kRun, setId, std::move(more));
}

void testMoreCandidatesThanTheDialogCanShowAreRefusedWholeAndFree()
{
    // The fixture already has three candidate paths. Exactly the maximum is shown, every one of them.
    {
        Fixture f;
        addCandidates(f, "set-9", kFollowUpMaximumCandidates - 3);
        const auto read = f.reader.read(kProject, kRun);
        check(read.success() && !read.value().candidateRefusal.has_value() && read.value().candidates.size() == kFollowUpMaximumCandidates
                && f.loader.calls == 1,
            "Exactly 256 candidates are all loaded and listed");
        check(read.value().candidates.back().relativePath == "src/b.cpp", "...including the last one in order, which a truncation would lose");
    }
    // One more is refused - whole, with a stable code, and before anything expensive or risky happens.
    {
        Fixture f;
        addCandidates(f, "set-9", kFollowUpMaximumCandidates - 2);
        const auto read = f.reader.read(kProject, kRun);
        check(read.success(), "the read itself is not a fault: " + codeOf(read.error()));
        check(read.value().candidateRefusal.has_value() && read.value().candidateRefusal->code == "follow_up.too_many_candidate_files",
            "257 unique candidates are refused with follow_up.too_many_candidate_files");
        check(read.value().candidates.empty() && read.value().snapshot.fingerprint.empty(),
            "...no candidate is handed over (never a cut-short list) and the workspace was not hashed");
        check(f.loader.calls == 0, "...and not one file was opened");
    }
    // Two spellings of one file are ONE candidate: they do not count twice against the maximum.
    {
        Fixture f;
        addCandidates(f, "set-9", kFollowUpMaximumCandidates - 3);
        f.proposals.add(kRun, "set-10", {change("src/b.cpp"), change("src/a.cpp")});
        const auto read = f.reader.read(kProject, kRun);
        check(read.success() && !read.value().candidateRefusal.has_value() && read.value().candidates.size() == kFollowUpMaximumCandidates,
            "A path named again by a later proposal is still one candidate");
    }
}

void testTheComposerRefusesATooLongCandidateListForFree()
{
    Fixture f;
    InMemoryProviderRepository providers;
    InMemoryModelRepository models;
    InMemoryRunDraftRepository drafts;
    InMemoryFollowUpPlanRepository requests([](const std::string&) -> std::optional<ProjectId> { return std::string(kProject); }, &drafts);
    Catalog catalog;
    check(providers.insert(providerOf(catalog)).success() && models.insert(modelOf(catalog)).success(), "setup: catalog");
    FollowUpPlanService service(requests, requests, f.reader, providers, models, drafts, [] { return at(0); });
    FollowUpIntent intent;
    intent.projectId = kProject;
    intent.sourceRunId = kRun;
    intent.goal = "Plan the next focused implementation step.";
    intent.maxDrafts = 2;
    intent.providerId = catalog.providerId;
    intent.modelId = catalog.modelId;

    // A request quoted while the list was complete (256) ...
    addCandidates(f, "set-9", kFollowUpMaximumCandidates - 3);
    const auto quoted = service.quote(intent);
    check(quoted.success(), "setup: a quote over exactly 256 candidates: " + codeOf(quoted.error()));
    check(service.describeFiles(kProject, kRun, FollowUpPlanKind::NextRun).value().size() == kFollowUpMaximumCandidates,
        "...and the dialog shows all 256");
    check(service.authorize(quoted.value().request.id).success(), "...is authorised");
    const auto authorized = requests.findById(quoted.value().request.id).value().value();
    check(authorized.state == FollowUpPlanRequestState::Authorized, "setup: authorized");

    // ... whose Run then gets a 257th candidate: the dialog refuses, for free, with the plain code.
    f.proposals.add(kRun, "set-11", {change("late.txt")});
    const auto described = service.describeFiles(kProject, kRun, FollowUpPlanKind::NextRun);
    check(!described.success() && described.error()->code == "follow_up.too_many_candidate_files",
        "The dialog refuses a 257-candidate Run instead of listing 256: " + codeOf(described.error()));

    // The stored request meets the same list at its claim: stale (files), and free - no provider call, no execution.
    InMemoryModelExecutionRepository executionStore;
    ModelExecutionService executions(executionStore);
    auto client = std::make_shared<ScriptedClient>();
    ScriptedFactory factory(client);
    FollowUpComposerProcessor processor(requests, f.reader, providers, models, factory, executions, [] { return at(0); });
    const auto claimed = processor.processAuthorized(quoted.value().request.id);
    check(!claimed.success() && claimed.error()->code == "follow_up.stale_files",
        "A stored request whose Run grew past 256 candidates is stale_files: " + codeOf(claimed.error()));
    const auto stored = requests.findById(quoted.value().request.id).value().value();
    check(stored.state == FollowUpPlanRequestState::Stale && !followUpRequestMayBeCharged(stored), "...a free one");
    check(client->calls() == 0, "...and nothing was sent to the provider");

    // With that request settled the slot is free, and a NEW quote refuses the same list with the plain code.
    const auto requote = service.quote(intent);
    check(!requote.success() && requote.error()->code == "follow_up.too_many_candidate_files",
        "A new quote refuses it: " + codeOf(requote.error()));
}

// The order of causes is a contract: a provider that changed is reported before the file list, even when both are true.
void testTheTooLongListComesAfterTheProviderInTheOrderOfCauses()
{
    Fixture f;
    InMemoryProviderRepository providers;
    InMemoryModelRepository models;
    InMemoryRunDraftRepository drafts;
    InMemoryFollowUpPlanRepository requests([](const std::string&) -> std::optional<ProjectId> { return std::string(kProject); }, &drafts);
    Catalog catalog;
    check(providers.insert(providerOf(catalog)).success() && models.insert(modelOf(catalog)).success(), "setup: catalog");
    FollowUpPlanService service(requests, requests, f.reader, providers, models, drafts, [] { return at(0); });
    FollowUpIntent intent;
    intent.projectId = kProject;
    intent.sourceRunId = kRun;
    intent.goal = "Plan the next focused implementation step.";
    intent.maxDrafts = 2;
    intent.providerId = catalog.providerId;
    intent.modelId = catalog.modelId;
    const auto quoted = service.quote(intent);
    check(quoted.success(), "setup: a quote: " + codeOf(quoted.error()));

    addCandidates(f, "set-9", kFollowUpMaximumCandidates);   // far past the maximum
    auto provider = providers.findById(catalog.providerId).value().value();
    provider.enabled = false;
    check(providers.update(provider).success(), "setup: the provider is disabled");
    const auto refused = service.authorize(quoted.value().request.id);
    check(!refused.success() && refused.error()->code == "follow_up.stale_provider",
        "Provider first, then files: " + codeOf(refused.error()));
}

// Hands its material over by MOVE, once: the test then owns the only copy of the file bytes until the code under test gets them.
class OneShotReader final : public IFollowUpSourceReader {
public:
    explicit OneShotReader(FollowUpSourceMaterial material) : material_(std::move(material)) {}
    [[nodiscard]] common::Result<FollowUpSourceMaterial> read(const ProjectId&, const RunId&) const override
    {
        return common::Result<FollowUpSourceMaterial>::ok(std::move(material_));
    }

private:
    mutable FollowUpSourceMaterial material_;
};

// The memory claim, measured: the file bytes (up to 64 MiB in all) are moved from the reader to the assembler, never copied on the way.
// A copy of one file's content is an allocation of exactly its size (+ the string's rounding); the probe counts those.
void testTheFileBytesAreMovedNotCopied()
{
    const std::size_t size = 250000;
    const std::string text = filler(size);
    const std::vector<FollowUpFileCandidate> files = {candidate("src/big.cpp", text)};
    check(files[0].content.size() == size, "setup: the candidate is the size the probe looks for");

    // The probe itself: the const& assembler copies the file once, the && assembler none.
    {
        FollowUpComposerContextRequest request;
        request.sourceRunId = kRun;
        request.goal = "Plan the next focused implementation step.";
        request.maxDrafts = 2;
        request.snapshot = snapshotOf(kRun, kProject);
        request.brief = briefOf(kRun, kProject);
        request.files = files;
        request.files[0].selected = true;

        AllocationProbe copyProbe(size, size + 64);
        const auto byCopy = assembleFollowUpComposerContext(static_cast<const FollowUpComposerContextRequest&>(request));
        const int copies = copyProbe.hits();
        check(byCopy.success() && copies == 1, "The probe sees the one copy the const& assembler makes: " + std::to_string(copies));

        AllocationProbe moveProbe(size, size + 64);
        const auto byMove = assembleFollowUpComposerContext(std::move(request));
        const int moves = moveProbe.hits();
        check(byMove.success() && moves == 0, "The && assembler copies no file bytes: " + std::to_string(moves));
        check(request.files.empty(), "...it consumed the files");
        check(byMove.value().contextFingerprint == byCopy.value().contextFingerprint, "...and builds the very same context");
    }

    // Admission: from the reader's material to the context there is no copy of the bytes, and none is kept in the admitted material.
    {
        InMemoryProviderRepository providers;
        InMemoryModelRepository models;
        Catalog catalog;
        check(providers.insert(providerOf(catalog)).success() && models.insert(modelOf(catalog)).success(), "setup: catalog");
        OneShotReader reader(materialOf(files));
        FollowUpIntent intent;
        intent.projectId = kProject;
        intent.sourceRunId = kRun;
        intent.goal = "Plan the next focused implementation step.";
        intent.maxDrafts = 2;
        intent.providerId = catalog.providerId;
        intent.modelId = catalog.modelId;
        const FollowUpAdmissionPorts ports {reader, providers, models};

        AllocationProbe probe(size, size + 64);
        const auto admitted = admitFollowUpIntent(ports, intent);
        const int copies = probe.hits();
        check(admitted.success(), "admission over the big file: " + codeOf(admitted.error()));
        check(copies == 0, "Admission copies no file bytes: " + std::to_string(copies));
        check(admitted.value().material.candidates.empty(), "...and the admitted material keeps none");
        check(admitted.value().context.files.size() == 1 && admitted.value().context.files[0].disposition == FollowUpExcerptDisposition::Included,
            "...while the file did reach the context");
    }

    // The dialog's file list the same way.
    {
        InMemoryProviderRepository providers;
        InMemoryModelRepository models;
        InMemoryRunDraftRepository drafts;
        InMemoryFollowUpPlanRepository requests([](const std::string&) -> std::optional<ProjectId> { return std::string(kProject); }, &drafts);
        OneShotReader reader(materialOf(files));
        FollowUpPlanService service(requests, requests, reader, providers, models, drafts, [] { return at(0); });
        AllocationProbe probe(size, size + 64);
        const auto described = service.describeFiles(kProject, kRun, FollowUpPlanKind::NextRun);
        const int copies = probe.hits();
        check(described.success() && described.value().size() == 1, "describeFiles over the big file: " + codeOf(described.error()));
        check(copies == 0, "describeFiles copies no file bytes: " + std::to_string(copies));
    }
}

void testTheReaderRefusesWhatIsNotThere()
{
    Fixture f;
    requireCode(f.reader.read("nope", kRun), "follow_up.project_not_found", "a project that does not exist");
    requireCode(f.reader.read(kProject, "run-nope"), "follow_up.run_not_found", "a Run that does not exist");

    // A Run without a brief.
    Fixture bare;
    Run second;
    second.id = "run-2";
    second.projectId = kProject;
    second.workflowId = kExecutionManifestWorkflowId;
    second.status = RunStatus::Completed;
    check(bare.runs.insertRun(second).success(), "setup: a Run without a brief");
    requireCode(bare.reader.read(kProject, "run-2"), "follow_up.outcome_missing", "a Run with no Outcome head");
    check(bare.loader.calls == 0, "...before any file was read");

    // A head that is stale against its Run is a fault, never a brief the caller can use.
    Fixture stale;
    auto run = stale.runs.findRun(kRun).value().value();
    run.status = RunStatus::Failed;
    check(stale.runs.updateRun(run).success(), "setup: the Run moved on");
    requireCode(stale.reader.read(kProject, kRun), "persistence.invalid_data", "a head that no longer matches its Run");
    check(stale.loader.calls == 0, "...and no file was read");
}

void testAFaultIsPassedOnUnchanged()
{
    {
        Fixture f;
        f.proposals.failure = common::Error {"persistence.read_failed", "the proposals are unreadable"};
        requireCode(f.reader.read(kProject, kRun), "persistence.read_failed", "a proposal read fault");
        check(f.loader.calls == 0, "...no file was read");
    }
    {
        Fixture f;
        std::error_code error;
        fs::remove_all(f.frozenWorkspace.path(), error);
        const auto read = f.reader.read(kProject, kRun);
        check(!read.success(), "A workspace that is gone is a fault (the snapshot cannot be built): " + codeOf(read.error()));
    }
    {
        // The snapshot is fine but the loader cannot use the root: a Run whose frozen workspace is a FILE.
        Fixture f;
        auto run = f.runs.findRun(kRun).value().value();
        const auto asFile = f.projectRoot.path() / "afile";
        f.projectRoot.write("afile", "x");
        run.workspacePath = asFile;
        check(f.runs.updateRun(run).success(), "setup: a workspace that is a file");
        check(!f.reader.read(kProject, kRun).success(), "A workspace that is not a directory is refused");
    }
}

// ---- the whole composer over the production reader ------------------------------------------------------------------------------

void testTheComposerRunsOverTheProductionReaderAndLoader()
{
    Fixture f;
    f.frozenWorkspace.write("src/c.cpp", "int c() { return 3; }\n");
    f.proposals.add(kRun, "set-3", {change("src/c.cpp")});

    // The composer, with a scripted provider and the in-memory stores of the 5C tests, but the PRODUCTION reader.
    InMemoryProviderRepository providers;
    InMemoryModelRepository models;
    InMemoryRunDraftRepository drafts;
    InMemoryFollowUpPlanRepository requests([](const std::string&) -> std::optional<ProjectId> { return std::string(kProject); }, &drafts);
    InMemoryModelExecutionRepository executionStore;
    ModelExecutionService executions(executionStore);
    Catalog catalog;
    check(providers.insert(providerOf(catalog)).success() && models.insert(modelOf(catalog)).success(), "setup: catalog");
    auto client = std::make_shared<ScriptedClient>();
    ScriptedFactory factory(client);
    std::atomic<std::int64_t> nowMs {0};
    const auto clock = [&] { return at(nowMs.load()); };
    FollowUpPlanService service(requests, requests, f.reader, providers, models, drafts, clock);
    FollowUpComposerProcessor processor(requests, f.reader, providers, models, factory, executions, clock);

    FollowUpIntent intent;
    intent.projectId = kProject;
    intent.sourceRunId = kRun;
    intent.goal = "Plan the next focused implementation step.";
    intent.maxDrafts = 2;
    intent.providerId = catalog.providerId;
    intent.modelId = catalog.modelId;

    const auto described = service.describeFiles(kProject, kRun, FollowUpPlanKind::NextRun);
    check(described.success(), "describeFiles over the production reader: " + codeOf(described.error()));
    std::map<std::string, FollowUpExcerptDisposition> dispositions;
    for (const auto& file : described.value()) dispositions[file.relativePath] = file.disposition;
    check(dispositions.size() == 4 && dispositions[".env"] == FollowUpExcerptDisposition::Ineligible
            && dispositions["src/a.cpp"] == FollowUpExcerptDisposition::Included && dispositions["src/c.cpp"] == FollowUpExcerptDisposition::Included,
        "The dialog sees every candidate; the credential file is ineligible and never selectable");

    const auto quoted = service.quote(intent);
    check(quoted.success(), "A quote over the production reader and loader: " + codeOf(quoted.error()));
    check(quoted.value().request.files.size() == 3, "Three files are pinned (the denied one is not)");
    const auto id = quoted.value().request.id;
    check(service.authorize(id).success(), "The authorisation re-reads the real workspace");
    client->push(okReply(replyOfPlanJson(planJsonOf('c', 2, true))));
    const auto done = processor.processAuthorized(id);
    check(done.success() && done.value().state == FollowUpPlanRequestState::PlanReady, "The claim and the call: " + codeOf(done.error()));
    const auto sent = client->lastRequest().userContent;
    check(sent.find("// frozen workspace") != std::string::npos && sent.find("int c() { return 3; }") != std::string::npos,
        "The provider got the content of the Run's frozen workspace");
    check(sent.find("TOKEN=abc") == std::string::npos && sent.find("project root, NOT the Run") == std::string::npos,
        "...and neither the credential file nor anything of the project root");
}

void testARealFileChangeBetweenQuoteAndAuthorisationIsAFreeStale()
{
    Fixture f;
    InMemoryProviderRepository providers;
    InMemoryModelRepository models;
    InMemoryRunDraftRepository drafts;
    InMemoryFollowUpPlanRepository requests([](const std::string&) -> std::optional<ProjectId> { return std::string(kProject); }, &drafts);
    Catalog catalog;
    check(providers.insert(providerOf(catalog)).success() && models.insert(modelOf(catalog)).success(), "setup: catalog");
    FollowUpPlanService service(requests, requests, f.reader, providers, models, drafts, [] { return at(0); });
    FollowUpIntent intent;
    intent.projectId = kProject;
    intent.sourceRunId = kRun;
    intent.goal = "Plan the next focused implementation step.";
    intent.maxDrafts = 2;
    intent.providerId = catalog.providerId;
    intent.modelId = catalog.modelId;

    const auto quoted = service.quote(intent);
    check(quoted.success(), "setup: a quote: " + codeOf(quoted.error()));
    f.frozenWorkspace.write("src/b.cpp", "int b() { return 999; }\n");   // a pinned file changes on disk
    const auto refused = service.authorize(quoted.value().request.id);
    check(!refused.success() && ai::application::isFollowUpStaleCode(refused.error()->code),
        "A pinned file that changed on disk is a stale: " + codeOf(refused.error()));
    const auto stored = requests.findById(quoted.value().request.id).value().value();
    check(stored.state == FollowUpPlanRequestState::Stale && !followUpRequestMayBeCharged(stored), "...and a free one");
}

} // namespace

int main()
{
    const std::vector<std::pair<const char*, std::function<void()>>> tests = {
        {"testTheCandidatePathsAreCanonicalUniqueOrderedAndBounded", testTheCandidatePathsAreCanonicalUniqueOrderedAndBounded},
        {"testTheLoaderClassifiesEveryFileBeforeAByteCanLeave", testTheLoaderClassifiesEveryFileBeforeAByteCanLeave},
        {"testTheLoaderNeverFollowsALink", testTheLoaderNeverFollowsALink},
        {"testASwapInTheGapNeverChangesWhatIsRead", testASwapInTheGapNeverChangesWhatIsRead},
        {"testWhatIsDoneWhileTheHandleIsOpenNeverChangesTheBytes", testWhatIsDoneWhileTheHandleIsOpenNeverChangesTheBytes},
        {"testTheLoaderRefusesAnUnavailableWorkspace", testTheLoaderRefusesAnUnavailableWorkspace},
        {"testTheReaderGivesTheMaterialOfTheSourceRun", testTheReaderGivesTheMaterialOfTheSourceRun},
        {"testTheReaderReadsNothingItWasNotEntitledTo", testTheReaderReadsNothingItWasNotEntitledTo},
        {"testMoreCandidatesThanTheDialogCanShowAreRefusedWholeAndFree", testMoreCandidatesThanTheDialogCanShowAreRefusedWholeAndFree},
        {"testTheComposerRefusesATooLongCandidateListForFree", testTheComposerRefusesATooLongCandidateListForFree},
        {"testTheTooLongListComesAfterTheProviderInTheOrderOfCauses", testTheTooLongListComesAfterTheProviderInTheOrderOfCauses},
        {"testTheFileBytesAreMovedNotCopied", testTheFileBytesAreMovedNotCopied},
        {"testTheReaderRefusesWhatIsNotThere", testTheReaderRefusesWhatIsNotThere},
        {"testAFaultIsPassedOnUnchanged", testAFaultIsPassedOnUnchanged},
        {"testTheComposerRunsOverTheProductionReaderAndLoader", testTheComposerRunsOverTheProductionReaderAndLoader},
        {"testARealFileChangeBetweenQuoteAndAuthorisationIsAFreeStale", testARealFileChangeBetweenQuoteAndAuthorisationIsAFreeStale},
    };
    int failures = 0;
    for (const auto& [name, test] : tests) {
        try {
            test();
        } catch (const std::exception& exception) {
            std::cerr << "FAILED " << name << ": " << exception.what() << '\n';
            ++failures;
        }
    }
    if (failures == 0) std::cout << "FollowUpSourceReader tests passed\n";
    return failures == 0 ? 0 : 1;
}
