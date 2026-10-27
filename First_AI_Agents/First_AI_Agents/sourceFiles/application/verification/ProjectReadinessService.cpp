#include "ai/application/verification/ProjectReadinessService.hpp"

#include "ai/application/workspace/FilesystemPathUtils.hpp"
#include "ai/application/Sha256.hpp"

#include <algorithm>
#include <fstream>
#include <set>
#include <sstream>

namespace ai::application {

namespace fs = std::filesystem;

const char* toString(ProjectReadinessState value) noexcept
{
    switch (value) {
    case ProjectReadinessState::Unverified: return "unverified";
    case ProjectReadinessState::Ready: return "ready";
    case ProjectReadinessState::Stale: return "stale";
    case ProjectReadinessState::Failed: return "failed";
    }
    return "unverified";
}

std::optional<ProjectReadinessState> projectReadinessStateFromString(std::string_view value) noexcept
{
    if (value == "unverified") return ProjectReadinessState::Unverified;
    if (value == "ready") return ProjectReadinessState::Ready;
    if (value == "stale") return ProjectReadinessState::Stale;
    if (value == "failed") return ProjectReadinessState::Failed;
    return std::nullopt;
}

namespace {

constexpr std::uintmax_t kMaxInputFileBytes = 2 * 1024 * 1024;
constexpr std::size_t kMaxVisitedEntries = 60'000;
constexpr int kMaxDepth = 16;

bool isDependencyManifest(const std::string& name)
{
    return name == "CMakeLists.txt" || name == "CMakePresets.json" || name == "CMakeUserPresets.json" || name == "vcpkg.json"
        || name == "vcpkg-configuration.json" || name == "conanfile.txt" || name == "conanfile.py";
}

bool isSkippedDirectoryName(const std::string& name)
{
    return name == ".git" || name == ".hg" || name == ".svn" || name == "node_modules" || name == "CMakeFiles" || name == "_deps"
        || name == ".vs" || name == ".idea" || name == ".cache";
}

std::optional<std::string> readBounded(const fs::path& path)
{
    std::error_code error;
    const auto size = fs::file_size(detail::longPath(path), error);
    if (error || size > kMaxInputFileBytes) return std::nullopt;
    std::ifstream stream(detail::longPath(path), std::ios::binary);
    if (!stream) return std::nullopt;
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    return buffer.str();
}

std::string rootKey(const fs::path& root)
{
    return detail::asciiLower(detail::genericUtf8(root));
}

std::string stripTrailingSlash(std::string text)
{
    while (text.size() > 1 && text.back() == '/') text.pop_back();
    return text;
}

} // namespace

ProjectReadinessService::ProjectReadinessService(IProjectReadinessRepository& repository, ToolchainProvider toolchain)
    : repository_(repository)
    , toolchain_(std::move(toolchain))
{
}

VerificationToolchainEnvironment ProjectReadinessService::toolchain() const
{
    return toolchain_ ? toolchain_() : VerificationToolchain::current();
}

std::shared_ptr<std::mutex> ProjectReadinessService::lockFor(const ProjectId& projectId)
{
    std::lock_guard lock(locksMutex_);
    auto& entry = locks_[projectId];
    if (!entry) entry = std::make_shared<std::mutex>();
    return entry;
}

common::Result<std::string> ProjectReadinessService::computeInputsFingerprint(const fs::path& projectRoot)
{
    using Result = common::Result<std::string>;
    std::error_code error;
    if (!fs::is_directory(detail::longPath(projectRoot), error)) {
        return Result::failure({"project_setup.root_unavailable", "The project folder is not available"});
    }

    std::set<std::string> lines; // sorted, de-duplicated "relative path \0 content hash"
    const auto addFile = [&](const fs::path& path, const std::string& label) {
        const auto content = readBounded(path);
        lines.insert(label + '\0' + (content.has_value() ? sha256Hex(*content) : std::string("unreadable")));
    };

    std::size_t visited = 0;
    bool truncated = false;
    fs::recursive_directory_iterator it(detail::longPath(projectRoot), fs::directory_options::skip_permission_denied, error);
    const fs::recursive_directory_iterator end;
    const auto rootLong = detail::longPath(projectRoot);
    while (!error && it != end) {
        const auto& entry = *it;
        if (++visited > kMaxVisitedEntries) {
            truncated = true;
            break;
        }
        std::error_code statusError;
        const std::string name = detail::genericUtf8(entry.path().filename());
        if (entry.is_directory(statusError)) {
            std::error_code cacheError;
            const bool buildTree = fs::exists(entry.path() / "CMakeCache.txt", cacheError);
            if (isSkippedDirectoryName(name) || buildTree || it.depth() >= kMaxDepth) it.disable_recursion_pending();
        } else if (entry.is_regular_file(statusError)) {
            const bool cmakeModule = entry.path().extension() == ".cmake";
            if (isDependencyManifest(name) || cmakeModule) {
                const auto relative = detail::genericUtf8(
                    detail::withoutLongPrefix(entry.path()).lexically_relative(detail::withoutLongPrefix(rootLong)));
                addFile(entry.path(), "f:" + relative);
            }
        }
        it.increment(error);
    }
    if (error) {
        return Result::failure({"project_setup.inputs_unreadable", "The project's configuration files could not be listed"});
    }

    // Preset includes and toolchain files may live outside the walk (an absolute toolchain file); they are inputs too.
    for (const auto& file : ProjectSetupReadiness::presetInputFiles(projectRoot)) {
        const auto relative = detail::withoutLongPrefix(file).lexically_relative(projectRoot);
        const bool inside = !relative.empty() && *relative.begin() != "..";
        addFile(file, (inside ? "f:" : "x:") + detail::genericUtf8(inside ? relative : file));
    }

    std::string material = truncated ? "truncated\n" : "complete\n";
    for (const auto& line : lines) material += line + '\n';
    return Result::ok(sha256Hex(material));
}

std::string ProjectReadinessService::fingerprintFor(const std::string& inputsFingerprint, const ProjectSetupSelection& selection)
{
    return sha256Hex(inputsFingerprint + '\n' + selection.configurePreset + '\n' + selection.buildPreset);
}

common::Result<std::optional<ProjectReadinessRecord>> ProjectReadinessService::current(const ProjectId& projectId) const
{
    return repository_.find(projectId);
}

common::Result<ProjectReadinessAssessment> ProjectReadinessService::assess(
    const ProjectId& projectId, const fs::path& projectRoot)
{
    using Result = common::Result<ProjectReadinessAssessment>;
    const auto found = repository_.find(projectId);
    if (!found.success()) return Result::failure(*found.error());

    ProjectReadinessAssessment assessment;
    assessment.record = found.value();
    if (!assessment.record.has_value()) {
        assessment.reason = "no_record";
        return Result::ok(std::move(assessment));
    }
    auto& record = *assessment.record;
    if (record.state != ProjectReadinessState::Ready) {
        assessment.reason = toString(record.state);
        return Result::ok(std::move(assessment));
    }

    std::string reason;
    if (stripTrailingSlash(rootKey(projectRoot)) != stripTrailingSlash(detail::asciiLower(record.projectRoot))) {
        reason = "project_root_changed";
    } else {
        const auto environment = toolchain();
        if (!environment.prepared) reason = "toolchain_unavailable";
        else if (environment.identity != record.toolchainIdentity) reason = "toolchain_changed";
    }
    if (reason.empty()) {
        std::error_code error;
        if (record.binaryDir.empty()
            || !fs::is_regular_file(detail::longPath(projectRoot / detail::pathFromUtf8(record.binaryDir) / "CMakeCache.txt"), error)) {
            reason = "build_tree_missing";
        }
    }
    if (reason.empty()) {
        const auto inputs = computeInputsFingerprint(projectRoot);
        if (!inputs.success()
            || fingerprintFor(inputs.value(), {record.configurePreset, record.buildPreset}) != record.inputsFingerprint) {
            reason = "configuration_changed";
        }
    }
    if (reason.empty()) {
        assessment.fresh = true;
        return Result::ok(std::move(assessment));
    }

    // Was ready, no longer provably so: say so durably. The selection is kept - it is the user's choice, not evidence.
    record.state = ProjectReadinessState::Stale;
    record.failureCode = "project_setup.stale";
    record.failureMessage = "The recorded setup no longer matches the project (" + reason + "). It will be checked again.";
    const auto saved = repository_.upsert(record);
    if (!saved.success()) return Result::failure(*saved.error());
    assessment.reason = reason;
    return Result::ok(std::move(assessment));
}

common::Result<void> ProjectReadinessService::selectPair(
    const ProjectId& projectId, const fs::path& projectRoot, const ProjectSetupSelection& selection)
{
    if (selection.empty()) {
        return common::Result<void>::failure({kProjectSetupPresetNotFound, "Select both a configure preset and a build preset"});
    }
    const auto lock = lockFor(projectId);
    std::lock_guard guard(*lock);
    ProjectReadinessRecord record;
    record.projectId = projectId;
    record.projectRoot = detail::genericUtf8(projectRoot);
    record.configurePreset = selection.configurePreset;
    record.buildPreset = selection.buildPreset;
    record.state = ProjectReadinessState::Unverified;
    record.checkedAt = std::chrono::system_clock::now();
    return repository_.upsert(record);
}

ProjectSetupCheck ProjectReadinessService::ensureReady(
    const ProjectId& projectId,
    const fs::path& projectRoot,
    IManifestVerificationCommandRunner& commandRunner,
    std::stop_token stopToken,
    bool forceRecheck)
{
    const auto lock = lockFor(projectId);
    std::lock_guard guard(*lock);

    ProjectSetupCheck outcome;
    const auto failWith = [&outcome](const char* code, std::string message) {
        outcome.ready = false;
        outcome.failureCode = code;
        outcome.failureMessage = std::move(message);
        return outcome;
    };

    if (forceRecheck) VerificationToolchain::forgetCachedResult();
    const auto assessed = assess(projectId, projectRoot);
    if (!assessed.success()) return failWith(kProjectSetupRecordNotSaved, assessed.error()->message);
    if (assessed.value().fresh && !forceRecheck) {
        const auto& record = *assessed.value().record;
        outcome.ready = true;
        outcome.selection = {record.configurePreset, record.buildPreset};
        outcome.binaryDirRelative = record.binaryDir;
        outcome.targets = record.targets;
        outcome.targetsComplete = record.targetsComplete;
        outcome.toolchainIdentity = record.toolchainIdentity;
        return outcome;
    }

    std::optional<ProjectSetupSelection> saved;
    if (assessed.value().record.has_value() && !assessed.value().record->configurePreset.empty()
        && !assessed.value().record->buildPreset.empty()) {
        saved = ProjectSetupSelection {assessed.value().record->configurePreset, assessed.value().record->buildPreset};
    }

    // The inputs as they are BEFORE the check. A result is only recorded as Ready when they are the same afterwards: a file
    // edited while cmake was running was not what the configure saw.
    const auto before = computeInputsFingerprint(projectRoot);

    auto checked = ProjectSetupReadiness::check(projectRoot, saved, commandRunner, stopToken, toolchain_);
    if (checked.failureCode == kProjectSetupCancelled || stopToken.stop_requested()) {
        // A cancelled or superseded check never writes: neither a false Ready nor a failure it did not establish.
        if (checked.ready) {
            checked.ready = false;
            checked.failureCode = kProjectSetupCancelled;
            checked.failureMessage = "The check was cancelled before it finished. Check again when you are ready.";
        }
        return checked;
    }

    ProjectReadinessRecord record;
    record.projectId = projectId;
    record.projectRoot = detail::genericUtf8(projectRoot);
    record.configurePreset = checked.selection.configurePreset;
    record.buildPreset = checked.selection.buildPreset;
    record.binaryDir = checked.binaryDirRelative;
    record.toolchainIdentity = checked.toolchainIdentity;
    record.checkedAt = std::chrono::system_clock::now();
    if (assessed.value().record.has_value()) record.lastSuccessAt = assessed.value().record->lastSuccessAt;

    if (checked.ready) {
        const auto after = computeInputsFingerprint(projectRoot);
        if (!before.success() || !after.success() || before.value() != after.value()) {
            checked.ready = false;
            checked.failureCode = "project_setup.changed_during_check";
            checked.failureMessage = "The project's configuration files changed while they were being checked, so the result "
                                     "was not recorded. Check again.";
            record.state = ProjectReadinessState::Failed;
        } else {
            record.state = ProjectReadinessState::Ready;
            record.inputsFingerprint = fingerprintFor(after.value(), checked.selection);
            record.targets = checked.targets;
            record.targetsComplete = checked.targetsComplete;
            record.lastSuccessAt = record.checkedAt;
        }
    } else {
        record.state = checked.failureCode == kProjectSetupSelectionRequired ? ProjectReadinessState::Unverified
                                                                              : ProjectReadinessState::Failed;
    }
    if (record.state != ProjectReadinessState::Ready) {
        record.failureCode = checked.failureCode;
        record.failureMessage = checked.failureMessage;
    }
    const auto saveResult = repository_.upsert(record);
    if (!saveResult.success()) {
        return failWith(kProjectSetupRecordNotSaved,
            "The result of the setup check could not be saved (" + saveResult.error()->message
                + "). Check again; if it keeps failing, the application database is unavailable.");
    }
    return checked;
}

} // namespace ai::application
