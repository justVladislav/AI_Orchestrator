#include "ai/application/workspace/FilesystemPathUtils.hpp"
#include "ai/application/verification/VerificationScratchPolicy.hpp"
#include "ai/application/verification/VerificationScratchService.hpp"
#include "ai/infrastructure/SqliteRepositories.hpp"

#include <sqlite3.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
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

// A short unique directory per test: the policy limits a scratch root to kMaxVerificationScratchRootLength characters on Windows, so
// every folder the tests create must fit (a long TEMP is the reason CTest runs with a short one).
struct TestDirectory {
    fs::path path;

    explicit TestDirectory(const std::string& tag)
    {
        static int counter = 0;
        path = fs::temp_directory_path()
            / ("aio-vs-" + tag + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count() % 1000000)
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

bool hasCode(const ai::common::Result<fs::path>& result, const char* code)
{
    return !result.success() && result.error()->code == code;
}

bool endsWithPrompt(const std::string& message)
{
    return message.find(kVerificationScratchChoosePrompt) != std::string::npos
        || message.find("created for you") != std::string::npos;
}

#ifdef _WIN32
int runCommand(const std::string& command)
{
    return std::system(("cmd /c " + command + " >nul 2>&1").c_str());
}

// Grants Everyone modify on the folder and everything created in it: the "shared folder" every user of the machine can write to.
void shareWithEveryone(const fs::path& directory)
{
    require(runCommand("icacls \"" + directory.string() + "\" /grant *S-1-1-0:(OI)(CI)M") == 0,
        "could not share the test folder");
}

bool makeJunction(const fs::path& link, const fs::path& target)
{
    return runCommand("mklink /J \"" + link.string() + "\" \"" + target.string() + "\"") == 0;
}
#endif

// ---------------------------------------------------------------------------------------------------------------------------------
// The policy
// ---------------------------------------------------------------------------------------------------------------------------------

void testPolicyRefusesAnUnusableSpelling()
{
    const auto relative = VerificationScratchPolicy::validate("relative/dir", std::nullopt, true);
    require(hasCode(relative, kVerificationScratchInvalidPathCode), "A relative path must be refused as invalid");
    require(endsWithPrompt(relative.error()->message), "A refusal must say what to do");

    TestDirectory base("sp");
    const auto dotdot = VerificationScratchPolicy::validate(base.path / ".." / "x", std::nullopt, true);
    require(hasCode(dotdot, kVerificationScratchInvalidPathCode), "A path with '..' must be refused as invalid");
    require(!fs::exists(base.path.parent_path() / "x"), "A refused path must not have been created");
    require(hasCode(VerificationScratchPolicy::validate({}, std::nullopt, true), kVerificationScratchInvalidPathCode),
        "An empty path must be refused as invalid");
}

#ifdef _WIN32
void testPolicyRefusesATooLongRootBeforeCreatingAnything()
{
    TestDirectory base("tl");
    fs::path longRoot = base.path;
    while (longRoot.native().size() <= kMaxVerificationScratchRootLength) longRoot /= std::string(24, 'a');
    const auto refused = VerificationScratchPolicy::validate(longRoot, std::nullopt, true);
    require(hasCode(refused, kVerificationScratchTooLongCode), "A root over the length limit must be refused as too long");
    require(refused.error()->message.find(std::to_string(kMaxVerificationScratchRootLength)) != std::string::npos
            && endsWithPrompt(refused.error()->message),
        "The refusal names the limit and says what to do");
    require(!fs::exists(detail::longPath(longRoot)), "A root that is too long must not be created");

    // The limit itself is accepted: a root of exactly kMaxVerificationScratchRootLength characters.
    fs::path exact = base.path;
    const std::size_t needed = kMaxVerificationScratchRootLength - exact.native().size();
    require(needed > 2, "the test directory itself is too long for this check");
    exact /= std::string(needed - 1, 'b');
    require(exact.native().size() == kMaxVerificationScratchRootLength, "the test path has the wrong length");
    const auto accepted = VerificationScratchPolicy::validate(exact, std::nullopt, true);
    require(accepted.success(), "A root of exactly the limit must be accepted: "
        + (accepted.success() ? std::string() : accepted.error()->message));
}
#endif

void testPolicyCreatesAMissingFolderPrivateAndLeavesNoProbe()
{
    TestDirectory base("cr");
    const fs::path root = base.path / "scratch";
    const auto created = VerificationScratchPolicy::validate(root, std::nullopt, true);
    require(created.success(), "A missing folder must be created: "
        + (created.success() ? std::string() : created.error()->message));
    require(fs::is_directory(root), "The folder must exist");
    std::string reason;
    require(VerificationScratchPolicy::isPrivateToCurrentUser(created.value(), reason),
        "A folder created by the policy must be private: " + reason);
    require(fs::is_empty(root), "The write probe must not be left behind");

    // Without createIfMissing a missing folder is reported, never created.
    const fs::path other = base.path / "not-created";
    require(hasCode(VerificationScratchPolicy::validate(other, std::nullopt, false), kVerificationScratchUnavailableCode),
        "A missing folder must be reported as unavailable when it may not be created");
    require(!fs::exists(other), "...and must not have been created");

    // A file where the folder should be.
    const fs::path file = base.path / "a-file";
    { std::ofstream(file) << "x"; }
    require(hasCode(VerificationScratchPolicy::validate(file, std::nullopt, true), kVerificationScratchUnavailableCode),
        "A path that is a file must be refused");
}

void testPolicyRefusesAFolderThatOverlapsTheWorkspace()
{
    TestDirectory base("ov");
    const fs::path workspace = base.path / "ws";
    fs::create_directories(workspace);
    const auto inside = VerificationScratchPolicy::validate(workspace / "scratch", workspace, true);
    require(hasCode(inside, kVerificationScratchOverlapCode), "A folder inside the workspace must be refused");
    require(!fs::exists(workspace / "scratch"), "A folder inside the workspace must not even be created");
    const auto same = VerificationScratchPolicy::validate(workspace, workspace, true);
    require(hasCode(same, kVerificationScratchOverlapCode), "The workspace itself must be refused");
    const auto containing = VerificationScratchPolicy::validate(base.path, workspace, true);
    require(hasCode(containing, kVerificationScratchOverlapCode), "A folder that contains the workspace must be refused");
    const auto disjoint = VerificationScratchPolicy::validate(base.path / "elsewhere", workspace, true);
    require(disjoint.success(), "A disjoint folder must be accepted");
}

#ifdef _WIN32
void testPolicyRefusesALinkOrJunction()
{
    TestDirectory base("jn");
    const fs::path target = base.path / "target";
    fs::create_directories(target);
    const fs::path link = base.path / "link";
    if (!makeJunction(link, target)) {
        std::cout << "[verification-scratch] junction could not be created here; the link check is not exercised" << std::endl;
        return;
    }
    const auto refused = VerificationScratchPolicy::validate(link, std::nullopt, true);
    require(hasCode(refused, kVerificationScratchUnsafeCode), "A junction must be refused as a link");
    require(endsWithPrompt(refused.error()->message), "A refusal must say what to do");
}

void testPolicyRefusesASharedFolderAndNeverTightensOneTheUserChose()
{
    TestDirectory base("sh");
    const fs::path shared = base.path / "shared";
    // Created by the policy, so it is private whatever the ACL of its parent is (a short TEMP at the root of a drive is not private).
    require(VerificationScratchPolicy::validate(shared, std::nullopt, true).success(), "setup: the folder is created private");
    std::string reason;
    require(VerificationScratchPolicy::isPrivateToCurrentUser(shared, reason), "setup: the folder is private to start with: " + reason);
    shareWithEveryone(shared);
    require(!VerificationScratchPolicy::isPrivateToCurrentUser(shared, reason) && !reason.empty(),
        "A folder Everyone can write to must not be private");

    // A folder the USER chose is refused with the reason - and left exactly as it was.
    const auto refused = VerificationScratchPolicy::validate(shared, std::nullopt, true);
    require(hasCode(refused, kVerificationScratchNotPrivateCode), "A shared folder must be refused as not private");
    require(refused.error()->message.find("created for you") != std::string::npos,
        "The refusal says that a new folder is created private");
    require(!VerificationScratchPolicy::isPrivateToCurrentUser(shared, reason),
        "A refused folder must not have been re-permissioned behind the user's back");

    // The application's OWN default folder is adopted: tightened instead of refused.
    const auto adopted = VerificationScratchPolicy::validate(shared, std::nullopt, true, /*adoptExistingDefault=*/true);
    require(adopted.success(), "The default folder must be tightened and accepted: "
        + (adopted.success() ? std::string() : adopted.error()->message));
    require(VerificationScratchPolicy::isPrivateToCurrentUser(shared, reason), "After adoption the folder is private: " + reason);
}
#endif

// ---------------------------------------------------------------------------------------------------------------------------------
// The privacy decision on MADE-UP system answers: a folder owned by another account, a NULL access list, or an entry of another
// account cannot be produced on a test machine without privileges, but the decision is a pure function of what the system reports.
// ---------------------------------------------------------------------------------------------------------------------------------

VerificationScratchAccessDescriptor privateFolder()
{
    VerificationScratchAccessDescriptor descriptor;
    descriptor.currentUserSid = "S-1-5-21-1-2-3-1001";
    descriptor.ownerSid = descriptor.currentUserSid;
    descriptor.allowedEntries = {{"S-1-5-18", 0x1F01FFu}, {"S-1-5-32-544", 0x1F01FFu}, {descriptor.currentUserSid, 0x1F01FFu}};
    return descriptor;
}

bool decides(const VerificationScratchAccessDescriptor& descriptor, std::string& reason)
{
    reason.clear();
    return VerificationScratchPolicy::evaluateWindowsAccess(descriptor, reason);
}

void testAccessDecisionOnMadeUpSystemAnswers()
{
    std::string reason;
    require(decides(privateFolder(), reason), "A folder only the user, SYSTEM and Administrators can use is private: " + reason);

    // The owner decides who can change the permissions at all.
    for (const char* owner : {"S-1-5-21-1-2-3-1002", "S-1-5-32-545", "S-1-1-0", "", "unreadable"}) {
        auto foreign = privateFolder();
        foreign.ownerSid = owner;
        require(!decides(foreign, reason) && reason == "it belongs to another account",
            std::string("A folder owned by '") + owner + "' must not be private: " + reason);
    }
    for (const char* owner : {"S-1-5-18", "S-1-5-32-544"}) {
        auto system = privateFolder();
        system.ownerSid = owner;
        require(decides(system, reason), std::string("A folder owned by ") + owner + " is private: " + reason);
    }

    auto nullAcl = privateFolder();
    nullAcl.aclPresent = false;
    require(!decides(nullAcl, reason) && reason.find("no access list") != std::string::npos, "A NULL access list means everyone can write");

    auto unknownUser = privateFolder();
    unknownUser.currentUserSid.clear();
    require(!decides(unknownUser, reason), "Without knowing the current account nothing is private");

    // Every write-like right of an OTHER account makes the folder shared, one bit at a time.
    const std::uint32_t writeBits[] = {0x2u, 0x4u, 0x10u, 0x40u, 0x100u, 0x10000u, 0x40000u, 0x80000u, 0x40000000u, 0x10000000u};
    for (const char* other : {"S-1-5-32-545", "S-1-5-11", "S-1-1-0", "S-1-5-21-1-2-3-1002", "unreadable"}) {
        for (const std::uint32_t bit : writeBits) {
            auto shared = privateFolder();
            shared.allowedEntries.push_back({other, bit});
            require(!decides(shared, reason) && reason == "other accounts can write to it",
                std::string("Right ") + std::to_string(bit) + " for '" + other + "' must make the folder shared");
        }
    }
    // Reading and listing (FILE_GENERIC_READ | FILE_GENERIC_EXECUTE = 0x1200A9) or no right at all is harmless.
    auto readers = privateFolder();
    readers.allowedEntries.push_back({"S-1-5-32-545", 0x1200A9u});
    readers.allowedEntries.push_back({"S-1-1-0", 0u});
    require(decides(readers, reason), "Read-only access for others does not share the folder: " + reason);

    // Accounts that may write without making it shared: the placeholder CREATOR OWNER and TrustedInstaller - but not just any service SID.
    auto placeholders = privateFolder();
    placeholders.allowedEntries.push_back({"S-1-3-0", 0x1F01FFu});
    placeholders.allowedEntries.push_back({"S-1-5-80-956008885-3418522649-1831038044-1853292631-2271478464", 0x1F01FFu});
    require(decides(placeholders, reason), "CREATOR OWNER and TrustedInstaller may write: " + reason);
    auto otherService = privateFolder();
    otherService.allowedEntries.push_back({"S-1-5-80-111-222-333-444-555", 0x2u});
    require(!decides(otherService, reason), "Another service account is not trusted");
}

// ---------------------------------------------------------------------------------------------------------------------------------
// The service (and the setting it keeps)
// ---------------------------------------------------------------------------------------------------------------------------------

void testServiceSetsClearsAndResolves()
{
    TestDirectory base("sv");
    InMemoryVerificationScratchSettingRepository repository;
    VerificationScratchService service(repository, base.path / "default");

    auto status = service.status();
    require(!status.configured.has_value() && status.source == VerificationScratchSource::Default
            && status.effective == base.path / "default" && status.usable,
        "Without a setting the default folder is the one in force, and a missing default is usable (created on first use)");
    require(!fs::exists(base.path / "default"), "status() must not create anything");

    // The default is created private when a verification first needs it.
    const auto first = service.resolve(std::nullopt);
    require(first.success() && first.value().source == VerificationScratchSource::Default
            && first.value().path == fs::weakly_canonical(base.path / "default"),
        "The default root must be resolved (and created)");
    require(fs::is_directory(base.path / "default"), "...and must now exist");

    // A user setting is validated, created private and stored as its canonical path.
    const fs::path chosen = base.path / "chosen";
    const auto set = service.set(chosen);
    require(set.success() && set.value().source == VerificationScratchSource::Setting, "A valid folder must be accepted");
    require(fs::is_directory(chosen), "A folder the user set is created");
    const auto loaded = service.configuredRoot();
    require(loaded.success() && loaded.value().has_value() && *loaded.value() == fs::weakly_canonical(chosen),
        "The canonical path is what is stored");
    const auto resolved = service.resolve(std::nullopt);
    require(resolved.success() && resolved.value().source == VerificationScratchSource::Setting
            && resolved.value().path == fs::weakly_canonical(chosen),
        "With a setting the setting is the root in force");

    // A refused folder leaves the previous setting in force.
    const auto refused = service.set(fs::path("relative"));
    require(!refused.success() && refused.error()->code == kVerificationScratchInvalidPathCode,
        "A refused folder must be reported with its own code");
    require(*service.configuredRoot().value() == fs::weakly_canonical(chosen), "A refused folder must leave the setting as it was");

    // Clearing goes back to the default.
    const auto cleared = service.set(std::nullopt);
    require(cleared.success() && cleared.value().source == VerificationScratchSource::Default, "Clearing selects the default");
    require(!service.configuredRoot().value().has_value(), "...and removes the stored value");
    require(service.resolve(std::nullopt).value().source == VerificationScratchSource::Default, "...and the default is in force");
}

void testServiceRefusesInsteadOfSilentlyFallingBackWhenTheChosenFolderIsNoLongerUsable()
{
    TestDirectory base("sr");
    InMemoryVerificationScratchSettingRepository repository;
    VerificationScratchService service(repository, base.path / "default");
    const fs::path chosen = base.path / "chosen";
    require(service.set(chosen).success(), "setup: the folder is accepted");

    // The workspace is later moved under the chosen folder: every verification of THAT project is refused (not run elsewhere).
    const fs::path workspace = chosen / "project";
    fs::create_directories(workspace);
    const auto refused = service.resolve(workspace);
    require(!refused.success() && refused.error()->code == kVerificationScratchOverlapCode,
        "A chosen folder that overlaps the workspace must refuse the verification");
    require(refused.error()->message.find("chosen in Settings") != std::string::npos,
        "The refusal says it is the folder from Settings that cannot be used");
    require(!fs::exists(base.path / "default"), "The verification must not have moved to the default folder");

    // The chosen folder disappears from disk: it is created again (private) on use; the default stays untouched.
    std::error_code ignored;
    fs::remove_all(chosen, ignored);
    const auto again = service.resolve(std::nullopt);
    require(again.success() && again.value().source == VerificationScratchSource::Setting,
        "A chosen folder that vanished is created again, private");
}

void testServiceRefusesAnUnusableDefaultWithAnInstruction()
{
    TestDirectory base("sd");
    InMemoryVerificationScratchSettingRepository repository;
    // A default that is a FILE cannot be used.
    const fs::path blocker = base.path / "blocker";
    { std::ofstream(blocker) << "x"; }
    VerificationScratchService service(repository, blocker);
    const auto refused = service.resolve(std::nullopt);
    require(!refused.success() && refused.error()->code == kVerificationScratchUnavailableCode,
        "An unusable default must refuse the verification");
    require(refused.error()->message.find("system's temporary folder") != std::string::npos
            && refused.error()->message.find(kVerificationScratchChoosePrompt) != std::string::npos,
        "The refusal says the temporary folder cannot be used and what to do");
    const auto status = service.status();
    require(!status.usable && status.problem.has_value() && !status.configured.has_value(),
        "status() reports the same problem");
    // Choosing a usable folder fixes it.
    require(service.set(base.path / "good").success(), "A usable folder can be chosen");
    require(service.resolve(std::nullopt).success(), "...and the verification is no longer refused");
}

#ifdef _WIN32
// The application's OWN default folder is tightened when it exists but is shared (it may have been created by an older version with an
// inherited ACL); a folder the USER chose is never touched - the verification is refused with the reason instead.
void testServiceAdoptsItsOwnDefaultFolderButNeverOneTheUserChose()
{
    TestDirectory base("sa");
    const fs::path shared = base.path / "default";
    require(VerificationScratchPolicy::validate(shared, std::nullopt, true).success(), "setup: the folder is created private");
    shareWithEveryone(shared);
    std::string reason;
    require(!VerificationScratchPolicy::isPrivateToCurrentUser(shared, reason), "setup: the folder is shared now");

    InMemoryVerificationScratchSettingRepository withoutSetting;
    VerificationScratchService defaultService(withoutSetting, shared);
    const auto adopted = defaultService.resolve(std::nullopt);
    require(adopted.success() && adopted.value().source == VerificationScratchSource::Default,
        "The shared default folder must be tightened and used: "
            + (adopted.success() ? std::string() : adopted.error()->message));
    require(VerificationScratchPolicy::isPrivateToCurrentUser(shared, reason), "...and is private afterwards: " + reason);

    // The same shared folder, but CHOSEN by the user (stored behind the service's back, as an older value or a changed folder would
    // be): refused, and left as it is.
    shareWithEveryone(shared);
    InMemoryVerificationScratchSettingRepository withSetting;
    require(withSetting.store(shared).success(), "setup: the stored value");
    VerificationScratchService chosenService(withSetting, base.path / "unused-default");
    const auto refused = chosenService.resolve(std::nullopt);
    require(!refused.success() && refused.error()->code == kVerificationScratchNotPrivateCode,
        "A shared folder the user chose must refuse the verification");
    require(!VerificationScratchPolicy::isPrivateToCurrentUser(shared, reason),
        "A folder the user chose must not be re-permissioned behind their back");
    require(!fs::exists(base.path / "unused-default"), "The verification must not have moved to the default folder");
}
#endif

void testServiceReportsStorageFailureInsteadOfUsingAnUnvalidatedFolder()
{
    class FailingRepository final : public IVerificationScratchSettingRepository {
    public:
        ai::common::Result<std::optional<fs::path>> load() const override
        {
            return ai::common::Result<std::optional<fs::path>>::failure({"storage.unavailable", "no database"});
        }
        ai::common::Result<void> store(const std::optional<fs::path>&) override
        {
            return ai::common::Result<void>::failure({"storage.unavailable", "no database"});
        }
    } repository;
    TestDirectory base("sf");
    VerificationScratchService service(repository, base.path / "default");
    const auto resolved = service.resolve(std::nullopt);
    require(!resolved.success() && resolved.error()->code == "storage.unavailable",
        "Without the setting store a verification is refused, not run in an unvalidated folder");
    require(!service.set(base.path / "x").success(), "...and a folder cannot be stored");
    require(!service.status().usable && service.status().problem.has_value(), "...and status() says so");
}

// ---------------------------------------------------------------------------------------------------------------------------------
// The SQLite setting (schema step 69)
// ---------------------------------------------------------------------------------------------------------------------------------

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

void testSqliteSettingPersistsAcrossReopenAndMigrates()
{
    TestDirectory base("db");
    const fs::path databasePath = base.path / "state" / "o.sqlite3";
    const fs::path chosen = base.path / "chosen";
    {
        const auto opened = ai::infrastructure::SqliteDatabase::open(databasePath);
        require(opened.success(), "Could not open the database");
        ai::infrastructure::SqliteVerificationScratchSettingRepository repository(opened.value());
        require(repository.load().success() && !repository.load().value().has_value(), "A new database has no setting");
        VerificationScratchService service(repository, base.path / "default");
        require(service.set(chosen).success(), "The folder is accepted");
    }
    {
        // A new connection (a restart) reads the same canonical path.
        const auto reopened = ai::infrastructure::SqliteDatabase::open(databasePath);
        require(reopened.success(), "Could not reopen the database");
        ai::infrastructure::SqliteVerificationScratchSettingRepository repository(reopened.value());
        const auto loaded = repository.load();
        require(loaded.success() && loaded.value().has_value() && *loaded.value() == fs::weakly_canonical(chosen),
            "The setting did not survive a restart");
        VerificationScratchService service(repository, base.path / "default");
        require(service.resolve(std::nullopt).value().source == VerificationScratchSource::Setting,
            "...and is in force after the restart");
        require(service.set(std::nullopt).success() && !repository.load().value().has_value(), "Clearing removes the stored value");
    }
    // A non-ASCII folder name survives as UTF-8 (the setting is a path, stored as text).
    const fs::path unicode = base.path / fs::path(u8"\u043f\u0430\u043f\u043a\u0430-\u6e2c\u8a66");
    {
        const auto opened = ai::infrastructure::SqliteDatabase::open(databasePath);
        ai::infrastructure::SqliteVerificationScratchSettingRepository repository(opened.value());
        require(repository.store(unicode).success(), "A non-ASCII path can be stored");
    }
    {
        const auto opened = ai::infrastructure::SqliteDatabase::open(databasePath);
        ai::infrastructure::SqliteVerificationScratchSettingRepository repository(opened.value());
        require(repository.load().value().has_value() && *repository.load().value() == unicode,
            "A non-ASCII path must survive unchanged");
    }

    // Step 69 is additive and repeatable: a database at v68 gets the (empty) table; a rewound v69 keeps its value.
    require(rawSql(databasePath, "DROP TABLE verification_scratch_setting; PRAGMA user_version = 68;").empty(),
        "Could not rewind to the v68 shape");
    {
        const auto migrated = ai::infrastructure::SqliteDatabase::open(databasePath);
        require(migrated.success(), "A v68 database did not migrate to v69: "
            + (migrated.success() ? std::string() : migrated.error()->message));
        ai::infrastructure::SqliteVerificationScratchSettingRepository repository(migrated.value());
        require(repository.load().success() && !repository.load().value().has_value(), "The migrated table is empty");
        require(repository.store(chosen).success(), "...and accepts a value");
    }
    require(rawSql(databasePath, "PRAGMA user_version = 68;").empty(), "Could not rewind the version only");
    {
        const auto again = ai::infrastructure::SqliteDatabase::open(databasePath);
        require(again.success(), "Step 69 must be repeatable over its own table");
        ai::infrastructure::SqliteVerificationScratchSettingRepository repository(again.value());
        require(repository.load().value().has_value() && *repository.load().value() == chosen,
            "Step 69 must keep an existing value");
    }
    require(!rawSql(databasePath, "INSERT INTO verification_scratch_setting (id, root_path, updated_at_ms) VALUES (2, 'x', 1);").empty(),
        "The table holds exactly one row (id = 1)");
    require(!rawSql(databasePath, "UPDATE verification_scratch_setting SET root_path = '' WHERE id = 1;").empty(),
        "An empty path is refused by the schema (NULL means the default)");
}

} // namespace

int main()
{
    try {
        testPolicyRefusesAnUnusableSpelling();
#ifdef _WIN32
        testPolicyRefusesATooLongRootBeforeCreatingAnything();
#endif
        testPolicyCreatesAMissingFolderPrivateAndLeavesNoProbe();
        testPolicyRefusesAFolderThatOverlapsTheWorkspace();
#ifdef _WIN32
        testPolicyRefusesALinkOrJunction();
        testPolicyRefusesASharedFolderAndNeverTightensOneTheUserChose();
#endif
        testAccessDecisionOnMadeUpSystemAnswers();
        testServiceSetsClearsAndResolves();
        testServiceRefusesInsteadOfSilentlyFallingBackWhenTheChosenFolderIsNoLongerUsable();
        testServiceRefusesAnUnusableDefaultWithAnInstruction();
#ifdef _WIN32
        testServiceAdoptsItsOwnDefaultFolderButNeverOneTheUserChose();
#endif
        testServiceReportsStorageFailureInsteadOfUsingAnUnvalidatedFolder();
        testSqliteSettingPersistsAcrossReopenAndMigrates();
    } catch (const std::exception& exception) {
        std::cerr << "VerificationScratch tests failed: " << exception.what() << std::endl;
        return 1;
    }
    std::cout << "VerificationScratch tests passed" << std::endl;
    return 0;
}
